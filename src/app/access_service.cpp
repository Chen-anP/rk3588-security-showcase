#include "app/access_service.hpp"
#include "access/pass_store.hpp"
#include "core/log.hpp"
#include "hw/pir.hpp"
#include "record/event_recorder.hpp"
#include "stream/mqtt_publisher.hpp"
#include <exception>
#include <fstream>
namespace rksec::app {
AccessService::AccessService(config::AppConfig cfg, stream::MqttPublisher *mqtt)
    : cfg_(std::move(cfg)), mqtt_(mqtt), worker_([this] { run(); })
{
}
AccessService::~AccessService()
{
    stop();
}
void AccessService::stop()
{
    stopping_.store(true);
    input_.close();
    if (worker_.joinable()) {
        worker_.join();
    }
    session_.store(0);
    auto closed = view();
    closed.persisted = false;
    closed.state = access::State::idle;
    publish(closed);
}
access::View AccessService::view() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return view_;
}
void AccessService::publish(const access::View &v)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    view_ = v;
    view_.pir_active = pir_active_.load();
}
void AccessService::pump(DetectPipeline &pipe, const DetectResult *result, const media::Image *preview,
                         PipelineClock::time_point preview_at)
{
    pipe.set_access_session(session_.load());
    Packet packet;
    if (result && result->ok) {
        packet.tracks = std::vector<int>{};
        packet.captured = result->captured;
        for (const auto &track : result->tracks) {
            if (track.box.cls == 0) {
                packet.tracks->push_back(track.id);
            }
        }
    }
    while (auto event = pipe.take_face_event()) {
        packet.faces.push_back(*event);
    }
    if (!packet.tracks && packet.faces.empty()) {
        return;
    }
    if (!packet.faces.empty() && preview && preview->width > 0 && preview->width <= 1920 && preview->height > 0 &&
        preview->height <= 1080 && preview->pixels.size() <= std::size_t{8294400}) {
        packet.preview = std::make_shared<media::Image>(*preview);
        packet.preview_at = preview_at;
    }
    input_.push(std::move(packet));
}
void AccessService::run()
{
    try {
        using Clock = PipelineClock;
        std::ifstream uuid_file("/proc/sys/kernel/random/uuid");
        std::string prefix;
        std::getline(uuid_file, prefix);
        std::unique_ptr<access::PassStore> store;
        std::shared_ptr<media::Image> snapshot;
        Clock::time_point snapshot_at{};
        access::Coordinator controller{[&](const access::FaceEvent &event, std::uint64_t session, std::string &error) {
                                           auto saving = view();
                                           saving.state = access::State::granted;
                                           saving.persisted = false;
                                           saving.error.clear();
                                           publish(saving);
                                           if (stopping_.load() || prefix.empty()) {
                                               error = "access stopping or request id unavailable";
                                               return false;
                                           }
                                           if (!store) {
                                               store = access::PassStore::open(cfg_.face.database, error);
                                           }
                                           if (!store) {
                                               return false;
                                           }
                                           access::NewPass pass;
                                           pass.request_id = prefix + ":" + std::to_string(session);
                                           pass.person_id = event.person_id;
                                           pass.occurred_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                  std::chrono::system_clock::now().time_since_epoch())
                                                                  .count();
                                           pass.score = event.score;
                                           if (snapshot && snapshot_at <= Clock::now() &&
                                               Clock::now() - snapshot_at <= std::chrono::milliseconds(500)) {
                                               std::string jpeg_error;
                                               if (!record::xrgb_to_jpeg(*snapshot, 85, pass.snapshot, jpeg_error)) {
                                                   pass.snapshot.clear();
                                               }
                                           }
                                           std::int64_t id = 0;
                                           bool inserted = false;
                                           const bool ok = store->add(pass, id, inserted, error);
                                           if (ok && inserted && mqtt_) {
                                               mqtt_->passage(pass.request_id, pass.occurred_ms);
                                           }
                                           return ok;
                                       },
                                       [] { return Clock::now(); }};
        std::unique_ptr<hw::PirInput> pir;
        hw::PirState pir_state;
        auto retry = Clock::now();
        std::uint64_t failures = 0;
        while (!stopping_.load()) {
            auto now = Clock::now();
            access::Input input;
            if (!pir && now >= retry) {
                std::string error;
                pir = hw::open_pir({cfg_.pir.chip, cfg_.pir.line, cfg_.pir.active_level == 1}, error);
                if (pir) {
                    pir_state = {};
                    const auto initial = pir->initial();
                    pir_state.update(initial, cfg_.pir.active_level == 1);
                    if (initial.status == hw::InputStatus::error) {
                        pir.reset();
                    } else {
                        input.motion = pir_state.active;
                    }
                } else if (failures++ < 5) {
                    LOG_W("access", "红外不可用，使用画面检测降级：%s", error.c_str());
                }
                retry = now + std::chrono::seconds(2);
            }
            if (pir) {
                const auto event = pir->wait(std::chrono::milliseconds(20));
                if (event.status == hw::InputStatus::error) {
                    pir.reset();
                    retry = Clock::now() + std::chrono::seconds(2);
                } else if (pir_state.update(event, cfg_.pir.active_level == 1) && pir_state.active) {
                    input.motion = true;
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            input.pir_available = static_cast<bool>(pir);
            pir_active_.store(pir && pir_state.known && pir_state.active);
            if (mqtt_) {
                mqtt_->pir(!pir || !pir_state.known ? stream::PirStatus::unavailable
                           : pir_state.active       ? stream::PirStatus::active
                                                    : stream::PirStatus::idle);
            }
            auto packet = input_.try_pop();
            now = Clock::now();
            if (packet) {
                if (packet->tracks && packet->captured <= now &&
                    now - packet->captured <= std::chrono::milliseconds(500)) {
                    input.present_tracks = packet->tracks;
                }
                snapshot = packet->preview;
                snapshot_at = packet->preview_at;
            } else {
                snapshot.reset();
            }
            input.motion = access::trigger_motion(input.motion,
                                                  pir && pir_state.known && pir_state.active && input.present_tracks &&
                                                      !input.present_tracks->empty(),
                                                  controller.state());
            auto state = controller.update(now, input);
            session_.store(controller.session());
            publish(state);
            if (packet) {
                for (const auto &face : packet->faces) {
                    if (stopping_.load()) {
                        break;
                    }
                    access::Input next;
                    next.pir_available = static_cast<bool>(pir);
                    state = controller.update(Clock::now(), next, face);
                    session_.store(controller.session());
                    publish(state);
                }
            }
            access::Input tick;
            tick.pir_available = static_cast<bool>(pir);
            publish(controller.update(Clock::now(), tick));
        }
    } catch (const std::exception &) {
        auto failed = view();
        failed.persisted = false;
        failed.error = "access worker failed";
        publish(failed);
        LOG_E("access", "门禁工作线程异常退出，监控继续运行");
    }
}
}  // namespace rksec::app
