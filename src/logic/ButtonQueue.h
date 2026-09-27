#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <string>

namespace mpgd {

// Bounded hand-off of button presses from the USB poll thread to the button
// thread. Button actions make blocking TNG calls (CmdExec, StartCode), so
// running them on the poll thread would stop pendant input being read while
// they run.
class ButtonQueue {
public:
    static constexpr size_t kCapacity = 32;

    // Queues a press. Returns false (and drops it) when the queue is full,
    // i.e. the button thread is stuck in a long TNG call.
    bool push(std::string buttonName) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (items_.size() >= kCapacity) return false;
            items_.push_back(std::move(buttonName));
        }
        cv_.notify_one();
        return true;
    }

    // Waits up to `timeout` for the next press.
    std::optional<std::string> popFor(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mtx_);
        if (!cv_.wait_for(lk, timeout, [&] { return !items_.empty(); })) {
            return std::nullopt;
        }
        std::string name = std::move(items_.front());
        items_.pop_front();
        return name;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return items_.size();
    }

private:
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<std::string> items_;
};

} // namespace mpgd
