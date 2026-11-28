#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace frame_sender {
class FrameNotification {
  public:
    using Clock = std::chrono::steady_clock;
    struct Snapshot {
        uint64_t sequence = 0;
        bool closed = false;
    };

    Snapshot snapshot() {
        std::lock_guard lock(mutex_);
        return state_;
    }
    void frame_arrived() {
        {
            std::lock_guard lock(mutex_);
            ++state_.sequence;
        }
        cv_.notify_one();
    }
    void close() {
        {
            std::lock_guard lock(mutex_);
            state_.closed = true;
        }
        cv_.notify_all();
    }
    Snapshot wait(uint64_t sequence, Clock::time_point deadline) {
        std::unique_lock lock(mutex_);
        cv_.wait_until(lock, deadline, [&] { return state_.closed || state_.sequence != sequence; });
        return state_;
    }

  private:
    std::mutex mutex_;
    std::condition_variable cv_;
    Snapshot state_;
};
} // namespace frame_sender
