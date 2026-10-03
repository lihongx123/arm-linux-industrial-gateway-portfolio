#pragma once
#include "device_driver.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
namespace mqmgateway::edge {
// Two shared workers for all polled devices, independent of epoll.
class AcquisitionScheduler {
public:
    explicit AcquisitionScheduler(std::vector<std::shared_ptr<IAcquisitionDriver>> drivers);
    ~AcquisitionScheduler() { stop(); }
    void start();
    void stop() noexcept;
    void appendMetrics(std::ostream& out) const;
private:
    std::vector<std::shared_ptr<IAcquisitionDriver>> drivers_;
    std::vector<std::thread> workers_;
    std::vector<std::chrono::steady_clock::time_point> due_;
    std::vector<bool> inFlight_;
    std::size_t cursor_{0};
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    bool stopping_{false};
    std::atomic<std::uint64_t> runs_{0}, late_{0}, errors_{0};
    void run();
};
}
