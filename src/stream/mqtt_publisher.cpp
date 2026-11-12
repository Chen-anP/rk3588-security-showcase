#include "stream/mqtt_publisher.hpp"

#include "core/log.hpp"
#include "stream/credentials.hpp"

#include <mosquitto.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <utility>

namespace rksec::stream {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

struct Library {
    const int status = mosquitto_lib_init();
    ~Library()
    {
        if (status == MOSQ_ERR_SUCCESS) {
            mosquitto_lib_cleanup();
        }
    }
};

struct Connection {
    bool connected = false;
    int rejection = 0;
    int ack = 0;
};

void connected(mosquitto *, void *data, int result)
{
    auto &state = *static_cast<Connection *>(data);
    state.connected = result == 0;
    state.rejection = result;
}

void acknowledged(mosquitto *, void *data, int id)
{
    static_cast<Connection *>(data)->ack = id;
}

const char *pir_payload(PirStatus status)
{
    switch (status) {
    case PirStatus::active:
        return R"({"state":"active"})";
    case PirStatus::idle:
        return R"({"state":"idle"})";
    case PirStatus::unavailable:
        return R"({"state":"unavailable"})";
    }
    return R"({"state":"unavailable"})";
}

constexpr std::string_view online = R"({"online":true})";
constexpr std::string_view offline = R"({"online":false})";

bool send(mosquitto *client, int &id, const std::string &topic, std::string_view payload, bool retain)
{
    return mosquitto_publish(client, &id, topic.c_str(), static_cast<int>(payload.size()), payload.data(), 1, retain) ==
           MOSQ_ERR_SUCCESS;
}

}  // namespace

std::unique_ptr<MqttPublisher> start_mqtt(const config::Mqtt &config)
{
    if (config.enabled) {
        try {
            return std::make_unique<MqttPublisher>(config);
        } catch (const std::exception &) {
            LOG_W("mqtt", "无法启动发布线程");
        }
    }
    return {};
}

MqttPublisher::MqttPublisher(config::Mqtt config) : config_(std::move(config)), worker_([this] { run(); }) {}

MqttPublisher::~MqttPublisher()
{
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        stopping_.store(true);
    }
    wake_.notify_one();
    worker_.join();
}

void MqttPublisher::pir(PirStatus state) noexcept
{
    pir_.store(state);
}

void MqttPublisher::passage(std::string_view event_id, std::int64_t occurred_ms) noexcept
{
    if (event_id.empty() || event_id.size() > 128 ||
        event_id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_:") !=
            std::string_view::npos) {
        return;
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto limit = reserved_ ? capacity - 1 : capacity;
    if (size_ == limit) {
        begin_ = (begin_ + 1) % capacity;
        --size_;
        ++dropped_;
    }
    auto &event = pending_[(begin_ + size_) % capacity];
    std::copy(event_id.begin(), event_id.end(), event.id.begin());
    event.id[event_id.size()] = '\0';
    event.occurred_ms = occurred_ms;
    ++size_;
}

void MqttPublisher::run()
{
    try {
        publish_loop();
    } catch (const std::exception &) {
        LOG_W("mqtt", "发布线程异常退出");
    }
}

void MqttPublisher::publish_loop()
{
    static const Library library;
    if (library.status != MOSQ_ERR_SUCCESS) {
        LOG_W("mqtt", "客户端库初始化失败");
        return;
    }

    const std::string client_id = "rksec-" + config_.device_id;
    const std::string base = "rksec/" + config_.device_id;
    const std::string availability = base + "/availability";
    const std::string pir_topic = base + "/pir";
    const std::string passage_topic = base + "/passage";
    std::optional<Event> event;
    auto retry = 1s;
    std::uint64_t reported_drops = 0;
    auto next_drop_report = Clock::now();

    while (!stopping_.load()) {
        Connection state;
        const std::unique_ptr<mosquitto, decltype(&mosquitto_destroy)> client{
            mosquitto_new(client_id.c_str(), true, &state), &mosquitto_destroy};
        Credentials credentials;
        std::string error;
        if (!client || !load_credentials(config_.credentials, credentials, error) ||
            credentials.password.find('\0') != std::string::npos ||
            mosquitto_username_pw_set(client.get(), credentials.user.c_str(), credentials.password.c_str()) !=
                MOSQ_ERR_SUCCESS ||
            mosquitto_tls_set(client.get(), config_.ca_file.c_str(), nullptr, nullptr, nullptr, nullptr) !=
                MOSQ_ERR_SUCCESS ||
            mosquitto_tls_opts_set(client.get(), 1, "tlsv1.2", nullptr) != MOSQ_ERR_SUCCESS ||
            mosquitto_tls_insecure_set(client.get(), false) != MOSQ_ERR_SUCCESS ||
            mosquitto_will_set(client.get(), availability.c_str(), static_cast<int>(offline.size()), offline.data(), 1,
                               true) != MOSQ_ERR_SUCCESS) {
            LOG_W("mqtt", "客户端初始化失败，请检查凭据权限、格式和 CA 文件");
        } else {
            mosquitto_connect_callback_set(client.get(), connected);
            mosquitto_publish_callback_set(client.get(), acknowledged);
            int result = mosquitto_connect(client.get(), config_.host.c_str(), config_.port, 15);
            const auto connect_deadline = Clock::now() + 5s;
            bool online_sent = false;
            std::optional<PirStatus> pir_sent;
            int inflight = 0;
            bool passage_inflight = false;
            auto ack_deadline = Clock::now();
            while (result == MOSQ_ERR_SUCCESS && !state.rejection && !stopping_.load()) {
                result = mosquitto_loop(client.get(), 100, 1);
                if (result != MOSQ_ERR_SUCCESS) {
                    break;
                }
                const auto now = Clock::now();
                if (!state.connected) {
                    if (now >= connect_deadline) {
                        break;
                    }
                    continue;
                }
                if (inflight && state.ack != inflight) {
                    if (now >= ack_deadline) {
                        break;
                    }
                    continue;
                }
                if (inflight && passage_inflight) {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    event.reset();
                    reserved_ = false;
                }
                if (online_sent) {
                    retry = 1s;
                }
                inflight = 0;
                passage_inflight = false;
                state.ack = 0;
                if (const auto count = dropped_.load(); now >= next_drop_report && count != reported_drops) {
                    LOG_W("mqtt", "队列累计淘汰 %llu 条通行事件", static_cast<unsigned long long>(count));
                    reported_drops = count;
                    next_drop_report = now + 30s;
                }
                const auto pir_status = pir_.load();
                bool sent = true;
                if (!online_sent) {
                    sent = send(client.get(), inflight, availability, online, true);
                    online_sent = sent;
                } else if (!pir_sent || *pir_sent != pir_status) {
                    sent = send(client.get(), inflight, pir_topic, pir_payload(pir_status), true);
                    pir_sent = pir_status;
                } else {
                    if (!event) {
                        const std::lock_guard<std::mutex> lock(mutex_);
                        if (size_) {
                            event = pending_[begin_];
                            begin_ = (begin_ + 1) % capacity;
                            --size_;
                            reserved_ = true;
                        }
                    }
                    if (event) {
                        const std::string payload = std::string{R"({"event_id":")"} + event->id.data() +
                                                    R"(","occurred_ms":)" + std::to_string(event->occurred_ms) +
                                                    R"(,"result":"simulated_grant"})";
                        sent = send(client.get(), inflight, passage_topic, payload, false);
                        passage_inflight = sent;
                    }
                }
                if (!sent) {
                    break;
                }
                ack_deadline = now + 5s;
            }
            if (stopping_.load() && state.connected && result == MOSQ_ERR_SUCCESS) {
                int offline_id = 0;
                if (send(client.get(), offline_id, availability, offline, true)) {
                    const auto deadline = Clock::now() + 500ms;
                    while (state.ack != offline_id && Clock::now() < deadline &&
                           mosquitto_loop(client.get(), 50, 1) == MOSQ_ERR_SUCCESS) {
                    }
                    if (state.ack == offline_id) {
                        mosquitto_disconnect(client.get());
                        mosquitto_loop_write(client.get(), 1);
                    }
                }
            } else if (!stopping_.load()) {
                LOG_W("mqtt", "连接或发布中断（客户端错误 %d，Broker 拒绝码 %d），%lld 秒后重试", result,
                      state.rejection, static_cast<long long>(retry.count()));
            }
        }
        // Wait without holding the producer lock during network calls.
        auto wait_lock = std::unique_lock<std::mutex>{mutex_};
        wake_.wait_for(wait_lock, retry, [this] { return stopping_.load(); });
        retry = std::min(30s, retry * 2);
    }
}

}  // namespace rksec::stream
