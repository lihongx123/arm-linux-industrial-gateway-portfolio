#include "alarm_event.hpp"
#include "diagnostic_json.hpp"

namespace mqmgateway::edge {
void AlarmLedger::append(const std::string& key, const std::string& action,
                         const std::string& severity, std::int64_t epochMs) {
    if (history_.size() == maxHistory_) history_.pop_front();
    history_.push_back({nextSequence_++, key, action, severity, epochMs});
}
void AlarmLedger::raise(const std::string& key, const std::string& severity, std::int64_t epochMs) {
    const auto it = active_.find(key);
    if (it == active_.end()) {
        active_.emplace(key, Active{severity, false});
        append(key, "raised", severity, epochMs);
    } else if (it->second.severity != severity) {
        it->second.severity = severity;
        it->second.acknowledged = false;
        append(key, "updated", severity, epochMs);
    }
}
void AlarmLedger::recover(const std::string& key, std::int64_t epochMs) {
    const auto it = active_.find(key);
    if (it == active_.end()) return;
    const auto severity = it->second.severity;
    active_.erase(it);
    append(key, "recovered", severity, epochMs);
}
void AlarmLedger::healthTransition(const HealthTransition& transition) {
    const auto key = "health/" + transition.deviceId;
    if (transition.to == HealthState::degraded) raise(key, "warning", transition.epochMs);
    else if (transition.to == HealthState::offline) raise(key, "critical", transition.epochMs);
    else if (transition.to == HealthState::online) recover(key, transition.epochMs);
}
void AlarmLedger::setSystemAlarm(const std::string& key, bool active, std::int64_t epochMs) {
    if (active) raise("system/" + key, "critical", epochMs);
    else recover("system/" + key, epochMs);
}
bool AlarmLedger::acknowledge(const std::string& key, std::int64_t epochMs) {
    const auto it = active_.find(key);
    if (it == active_.end() || it->second.acknowledged) return false;
    it->second.acknowledged = true;
    append(key, "acknowledged", it->second.severity, epochMs);
    return true;
}
std::vector<AlarmEvent> AlarmLedger::eventsSince(std::uint64_t sequence) const {
    std::vector<AlarmEvent> events;
    for (const auto& event : history_)
        if (event.sequence > sequence) events.push_back(event);
    return events;
}
void AlarmLedger::appendMetrics(std::ostream& out) const {
    out << "{\"active\":" << active_.size() << ",\"events_total\":" << eventCount()
        << ",\"history_stored\":" << history_.size() << ",\"recent\":[";
    const auto begin = history_.size() > 8 ? history_.size() - 8 : 0;
    for (std::size_t index = begin; index < history_.size(); ++index) {
        if (index > begin) out << ',';
        const auto& event = history_[index];
        out << "{\"sequence\":" << event.sequence << ",\"key\":\""
            << diagnosticJsonEscape(event.key) << "\",\"action\":\"" << event.action
            << "\",\"severity\":\"" << event.severity << "\",\"epoch_ms\":"
            << event.epochMs << '}';
    }
    out << "]}";
}
}  // namespace mqmgateway::edge
