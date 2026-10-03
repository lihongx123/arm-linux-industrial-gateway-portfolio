#include "point_mapper.hpp"
#include <cmath>
#include <iomanip>
#include <sstream>
namespace mqmgateway::edge {
bool PointMapper::apply(UnifiedMessageV2& message, const PointDefinition& point) {
    if (message.deviceId != point.deviceId || (!message.pointId.empty() && message.pointId != point.id))
        return false;
    message.pointId = point.id;
    const auto& bytes = message.pointRawPayload.empty() ? message.rawPayload : message.pointRawPayload;
    if (bytes.size() > 4096 || !std::isfinite(point.scale) || !std::isfinite(point.offset)) return false;
    std::ostringstream raw;
    raw << std::hex << std::setfill('0');
    for (auto byte : bytes) raw << std::setw(2) << unsigned(byte);
    message.rawValue = raw.str();
    if (message.dataType != iot::DataType::telemetry) return true;
    switch (point.type) {
    case PointValueType::bytes:
        message.cookedValue = message.rawValue; return true;
    case PointValueType::boolean:
        if (bytes.size() != 1) return false;
        message.cookedValue = bytes[0] ? "true" : "false"; return true;
    case PointValueType::integer: {
        if (bytes.empty() || bytes.size() > 8) return false;
        std::uint64_t value = 0;
        for (auto byte : bytes) value = (value << 8) | byte;
        const double mapped = double(value) * point.scale + point.offset;
        if (!std::isfinite(mapped)) return false;
        std::ostringstream out;
        out << std::setprecision(15) << mapped;
        message.cookedValue = out.str(); return true;
    }
    case PointValueType::floating:
        // Floating encodings require an explicit declared wire format; no guessing.
        return false;
    case PointValueType::text:
        for (auto byte : bytes) if (byte < 32 || byte > 126) return false;
        message.cookedValue.assign(bytes.begin(), bytes.end()); return true;
    }
    return false;
}
}
