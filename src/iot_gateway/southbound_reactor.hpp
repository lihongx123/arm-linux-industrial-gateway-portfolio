#pragma once
#include "device_driver.hpp"
#include <atomic>
#include <memory>
#include <vector>

namespace mqmgateway::iot {
// Readiness/timer dispatch only. Protocol state belongs to IDeviceDriver implementations.
class SouthboundReactor {
public:
    explicit SouthboundReactor(std::vector<std::shared_ptr<edge::IEventDrivenDriver>> drivers);
    ~SouthboundReactor();
    SouthboundReactor(const SouthboundReactor&) = delete;
    SouthboundReactor& operator=(const SouthboundReactor&) = delete;
    void run(const std::atomic<bool>& running);
    void wake();
private:
    struct Entry {
        std::shared_ptr<edge::IEventDrivenDriver> driver;
        int fd;
        std::uint32_t events;
        std::uint64_t token{0};
    };
    std::vector<Entry> entries_;
    int epoll_{-1}, wake_{-1};
    std::uint64_t nextToken_{1};
    void syncInterest(Entry& entry);
};
}
