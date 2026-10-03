#pragma once

#include "unified_message_v2.hpp"
#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>

namespace mqmgateway::northbound {

struct PublishResult {
    bool accepted{false};
    std::string error;
};

struct AdapterMetrics {
    std::uint64_t published{0};
    std::uint64_t publishFailed{0};
    std::uint64_t dropped{0};
    std::uint64_t inboundCommands{0};
    bool healthy{false};
    std::size_t queueDepth{0};
    std::size_t queuePeak{0};
    std::size_t pendingPublishes{0};
};

class INorthboundAdapter {
public:
    using CommandHandler = std::function<void(edge::UnifiedMessageV2, std::string)>;
    virtual ~INorthboundAdapter() = default;
    virtual std::string id() const = 0;
    virtual bool start() = 0;
    virtual void stop() noexcept = 0;
    virtual bool healthy() const = 0;
    // Must be bounded and must not retain or mutate the caller's message.
    virtual PublishResult publish(const edge::UnifiedMessageV2& message) = 0;
    virtual void setCommandHandler(CommandHandler handler) = 0;
    virtual AdapterMetrics metrics() const = 0;
};

}  // namespace mqmgateway::northbound
