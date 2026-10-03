#pragma once

#include "unified_message.hpp"
#include "pipeline_metrics.hpp"

#include <atomic>
#include <chrono>
#include <string>

namespace mqmgateway::iot {

class CanSocket {
public:
    explicit CanSocket(std::string interfaceName, bool observe = false);
    ~CanSocket();
    CanSocket(const CanSocket&) = delete;
    CanSocket& operator=(const CanSocket&) = delete;

    void open();
    void close();
    bool isOpen() const;
    int nativeHandle() const { return socket_; }
    bool receiveReady(UnifiedMessage& message);
    bool send(const UnifiedMessage& message);
    void writeMetrics(std::ostream& out) const;

private:
    std::string interfaceName_;
    int socket_{-1};
    bool observe_{false};
    std::atomic<std::uint64_t> received_{0}, parsed_{0}, rejected_{0}, readErrors_{0};
    StageLatency parseLatency_;
};

}  // namespace mqmgateway::iot
