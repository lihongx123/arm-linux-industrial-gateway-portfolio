#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace mqmgateway::edge {

enum class PointValueType { boolean, integer, floating, text, bytes };

struct PointDefinition {
    std::string deviceId;
    std::string id;
    PointValueType type{PointValueType::bytes};
    std::string unit;
    bool writable{false};
    std::uint32_t address{0};
    double scale{1.0};
    double offset{0.0};
};

class PointRegistry {
public:
    bool add(PointDefinition point);
    std::optional<PointDefinition> find(const std::string& deviceId, const std::string& pointId) const;
    bool remove(const std::string& deviceId, const std::string& pointId);

private:
    static std::string key(const std::string& deviceId, const std::string& pointId);
    mutable std::mutex mutex_;
    std::unordered_map<std::string, PointDefinition> points_;
};

}  // namespace mqmgateway::edge
