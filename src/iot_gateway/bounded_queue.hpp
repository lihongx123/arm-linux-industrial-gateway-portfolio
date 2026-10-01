#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

namespace mqmgateway::iot {

struct QueueMetrics {
    std::uint64_t enqueued{0};
    std::uint64_t dequeued{0};
    std::uint64_t rejected{0};
    std::size_t currentDepth{0};
    std::size_t peakDepth{0};
    double processingLatencyMs{0.0};
};

template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(const std::size_t capacity) : capacity_(capacity) {}

    bool tryPush(T item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_ || queue_.size() >= capacity_) {
            ++metrics_.rejected;
            return false;
        }
        queue_.push_back(std::move(item));
        ++metrics_.enqueued;
        metrics_.currentDepth = queue_.size();
        if (metrics_.currentDepth > metrics_.peakDepth) {
            metrics_.peakDepth = metrics_.currentDepth;
        }
        condition_.notify_one();
        return true;
    }

    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this] { return stopped_ || !queue_.empty(); });
        if (queue_.empty()) {
            return std::nullopt;
        }
        T item = std::move(queue_.front());
        queue_.pop_front();
        ++metrics_.dequeued;
        metrics_.currentDepth = queue_.size();
        return item;
    }

    void observeLatency(const double milliseconds) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto count = static_cast<double>(metrics_.dequeued);
        metrics_.processingLatencyMs += (milliseconds - metrics_.processingLatencyMs) / (count > 0 ? count : 1.0);
    }

    void stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        condition_.notify_all();
    }

    QueueMetrics metrics() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return metrics_;
    }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<T> queue_;
    QueueMetrics metrics_;
    bool stopped_{false};
};

}  // namespace mqmgateway::iot
