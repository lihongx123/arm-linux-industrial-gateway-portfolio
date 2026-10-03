#pragma once

#include "unified_message_v2.hpp"
#include "point_registry.hpp"
#include <optional>

#include <cstdint>
#include <functional>
#include <ostream>
#include <string>

namespace mqmgateway::edge {

class IDeviceDriver {
public:
    using Emit = std::function<void(UnifiedMessageV2)>;
    virtual ~IDeviceDriver() = default;
    virtual std::string id() const = 0;
    virtual void setMessageSink(Emit emit) = 0;
    virtual void start() = 0;
    // Must finish callbacks before returning; GatewayCore may be destroyed after stop.
    virtual void stop() noexcept = 0;
    virtual bool submit(const UnifiedMessageV2& command) = 0;
    virtual bool acceptsDevice(const std::string&) const { return false; }
    virtual std::string defaultCommand() const { return {}; }
    virtual std::optional<PointDefinition> describePoint(const UnifiedMessageV2&) const { return std::nullopt; }
    // Appends compatibility metric members (including the leading comma).
    virtual void appendMetrics(std::ostream&) const {}
};

// Optional capability for socket/serial drivers, not required by acquisition drivers.
class IEventDrivenDriver {
public:
    virtual ~IEventDrivenDriver() = default;
    virtual int nativeHandle() const = 0;
    virtual void onReady(std::uint32_t events) = 0;
    virtual std::uint32_t desiredEvents() const = 0;
    virtual void onTick(std::chrono::steady_clock::time_point) {}
    virtual void setWake(std::function<void()> wake) { (void)wake; }
};

class IAcquisitionDriver {
public:
    virtual ~IAcquisitionDriver() = default;
    virtual std::chrono::milliseconds interval() const = 0;
    virtual void acquire(std::chrono::steady_clock::time_point now) = 0;
};

}  // namespace mqmgateway::edge
