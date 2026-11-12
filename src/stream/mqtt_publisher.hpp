#pragma once
#include "core/config.hpp"
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>

namespace rksec::stream {

class MqttPublisher;
std::unique_ptr<MqttPublisher> start_mqtt(const config::Mqtt &config);

enum class PirStatus { unavailable, idle, active };

// The network thread owns the client. Producers only update the bounded mailbox.
class MqttPublisher {
  public:
    static constexpr std::size_t capacity = 128;
    explicit MqttPublisher(config::Mqtt config);
    ~MqttPublisher();
    MqttPublisher(const MqttPublisher &) = delete;
    MqttPublisher &operator=(const MqttPublisher &) = delete;

    void pir(PirStatus state) noexcept;
    void passage(std::string_view event_id, std::int64_t occurred_ms) noexcept;
    std::uint64_t dropped() const noexcept { return dropped_.load(); }

  private:
    struct Event {
        std::array<char, 129> id{};
        std::int64_t occurred_ms = 0;
    };
    void run();
    void publish_loop();

    config::Mqtt config_;
    std::atomic<PirStatus> pir_{PirStatus::unavailable};
    std::atomic<bool> stopping_{false};
    std::atomic<std::uint64_t> dropped_{0};
    std::mutex mutex_;
    std::condition_variable wake_;
    std::array<Event, capacity> pending_{};
    std::size_t begin_ = 0;
    std::size_t size_ = 0;
    // Reserved event stays in the publisher until a PUBACK arrives.
    bool reserved_ = false;
    std::thread worker_;
};

}  // namespace rksec::stream
