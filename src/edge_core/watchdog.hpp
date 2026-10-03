#pragma once

#include "time_service.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <ostream>

namespace mqmgateway::edge {

// An observer only. It never kills or restarts a process, and an empty queue
// is idle rather than stalled.
class QueueWatchdog {
public:
    enum class Transition { stalled, recovered };
    explicit QueueWatchdog(std::chrono::milliseconds threshold);
    std::optional<Transition> observe(std::size_t depth, std::uint64_t dequeued,
                                      TimeService::Monotonic::time_point now);
    void appendMetrics(std::ostream& out, const char* name = "queue_watchdog") const;
private:
    std::chrono::milliseconds threshold_;
    mutable std::mutex mutex_;
    TimeService::Monotonic::time_point lastProgress_{};
    std::uint64_t lastDequeued_{0}, stallEvents_{0}, recoveries_{0};
    std::size_t lastDepth_{0};
    bool initialized_{false}, stalled_{false};
};

}  // namespace mqmgateway::edge
