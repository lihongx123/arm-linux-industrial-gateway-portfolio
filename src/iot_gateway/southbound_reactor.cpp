#include "southbound_reactor.hpp"
#include <cerrno>
#include <iostream>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace mqmgateway::iot {
SouthboundReactor::SouthboundReactor(std::vector<std::shared_ptr<edge::IEventDrivenDriver>> drivers) {
    try {
        epoll_ = epoll_create1(EPOLL_CLOEXEC);
        wake_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (epoll_ < 0 || wake_ < 0) throw std::runtime_error("southbound epoll/eventfd");
        const auto add = [this](int fd, std::uint32_t events) {
            epoll_event e{};
            e.events = events;
            e.data.u64 = 0;
            if (epoll_ctl(epoll_, EPOLL_CTL_ADD, fd, &e)) throw std::runtime_error("southbound registration");
        };
        add(wake_, EPOLLIN);
        for (auto& driver : drivers) {
            entries_.push_back({driver, -1, 0});
            syncInterest(entries_.back());
            driver->setWake([this] { wake(); });
        }
        std::cerr << "SOUTHBOUND pid=" << getpid() << " epoll=" << epoll_
                  << " drivers=" << entries_.size() << "\n";
    } catch (...) {
        for (auto& entry : entries_) entry.driver->setWake({});
        if (epoll_ >= 0) close(epoll_);
        if (wake_ >= 0) close(wake_);
        throw;
    }
}
SouthboundReactor::~SouthboundReactor() {
    for (auto& entry : entries_) entry.driver->setWake({});
    if (epoll_ >= 0) close(epoll_);
    if (wake_ >= 0) close(wake_);
}
void SouthboundReactor::wake() {
    const std::uint64_t one = 1;
    const auto n = write(wake_, &one, sizeof(one));
    (void)n;
}
void SouthboundReactor::syncInterest(Entry& entry) {
    const auto fd = entry.driver->nativeHandle();
    const auto events = entry.driver->desiredEvents();
    if (fd != entry.fd) {
        if (entry.fd >= 0 && epoll_ctl(epoll_, EPOLL_CTL_DEL, entry.fd, nullptr) &&
            errno != EBADF && errno != ENOENT)
            throw std::runtime_error("southbound removal");
        entry.fd = fd;
        entry.events = events;
        entry.token = nextToken_++;
        if (fd >= 0) {
            epoll_event e{};
            e.events = events;
            e.data.u64 = entry.token;
            if (epoll_ctl(epoll_, EPOLL_CTL_ADD, fd, &e))
                throw std::runtime_error("southbound dynamic registration");
        }
        return;
    }
    if (fd < 0) return;
    if (events == entry.events) return;
    epoll_event e{};
    e.events = events;
    e.data.u64 = entry.token;
    if (epoll_ctl(epoll_, EPOLL_CTL_MOD, entry.fd, &e)) throw std::runtime_error("southbound interest update");
    entry.events = events;
}
void SouthboundReactor::run(const std::atomic<bool>& running) {
    epoll_event events[8];
    while (running) {
        const auto now = std::chrono::steady_clock::now();
        for (auto& entry : entries_) {
            entry.driver->onTick(now);
            syncInterest(entry);
        }
        const int count = epoll_wait(epoll_, events, 8, 10);
        if (count < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("southbound epoll_wait");
        }
        for (int i = 0; i < count; ++i) {
            if (events[i].data.u64 == 0) {
                std::uint64_t value;
                while (read(wake_, &value, sizeof(value)) > 0) {}
                continue;
            }
            for (auto& entry : entries_) {
                if (entry.fd >= 0 && entry.token == events[i].data.u64) {
                    entry.driver->onReady(events[i].events);
                    syncInterest(entry);
                    break;
                }
            }
        }
    }
}
}
