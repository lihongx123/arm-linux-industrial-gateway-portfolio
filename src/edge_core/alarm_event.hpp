#pragma once

#include "device_health.hpp"
#include <cstdint>
#include <deque>
#include <map>
#include <ostream>
#include <string>
#include <vector>

namespace mqmgateway::edge {

struct AlarmEvent {
    std::uint64_t sequence{0};
    std::string key;
    std::string action;
    std::string severity;
    std::int64_t epochMs{0};
};

// Edge-triggered, acknowledged alarms. Stored history is deliberately bounded.
class AlarmLedger {
public:
    void healthTransition(const HealthTransition& transition);
    void setSystemAlarm(const std::string& key, bool active, std::int64_t epochMs);
    bool acknowledge(const std::string& key, std::int64_t epochMs);
    std::size_t activeCount() const { return active_.size(); }
    std::size_t historySize() const { return history_.size(); }
    std::uint64_t eventCount() const { return nextSequence_ - 1; }
    std::vector<AlarmEvent> eventsSince(std::uint64_t sequence) const;
    void appendMetrics(std::ostream& out) const;
private:
    struct Active { std::string severity; bool acknowledged{false}; };
    void raise(const std::string& key, const std::string& severity, std::int64_t epochMs);
    void recover(const std::string& key, std::int64_t epochMs);
    void append(const std::string& key, const std::string& action,
                const std::string& severity, std::int64_t epochMs);
    std::map<std::string, Active> active_;
    std::deque<AlarmEvent> history_;
    std::uint64_t nextSequence_{1};
    static constexpr std::size_t maxHistory_ = 256;
};

}  // namespace mqmgateway::edge
