#include "watchdog.hpp"
#include <stdexcept>

namespace mqmgateway::edge {
QueueWatchdog::QueueWatchdog(std::chrono::milliseconds threshold) : threshold_(threshold) {
    if (threshold_.count() <= 0) throw std::invalid_argument("queue watchdog threshold must be positive");
}
std::optional<QueueWatchdog::Transition> QueueWatchdog::observe(
    std::size_t depth, std::uint64_t dequeued, TimeService::Monotonic::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || dequeued != lastDequeued_ || depth == 0 || lastDepth_ == 0) {
        initialized_ = true;
        lastProgress_ = now;
        lastDequeued_ = dequeued;
        lastDepth_ = depth;
        if (stalled_) {
            stalled_ = false;
            ++recoveries_;
            return Transition::recovered;
        }
        return std::nullopt;
    }
    lastDepth_ = depth;
    if (!stalled_ && now - lastProgress_ >= threshold_) {
        stalled_ = true;
        ++stallEvents_;
        return Transition::stalled;
    }
    return std::nullopt;
}
void QueueWatchdog::appendMetrics(std::ostream& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    out << ",\"queue_watchdog\":{\"stalled\":" << (stalled_ ? "true" : "false")
        << ",\"stall_events\":" << stallEvents_ << ",\"recoveries\":" << recoveries_
        << ",\"last_depth\":" << lastDepth_ << ",\"threshold_ms\":"
        << threshold_.count() << '}';
}
}  // namespace mqmgateway::edge
