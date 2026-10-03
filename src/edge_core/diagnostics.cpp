#include "diagnostics.hpp"
#include <sstream>

namespace mqmgateway::edge {
DiagnosticsManager::DiagnosticsManager(HealthConfig config, TimeService time)
    : time_(std::move(time)), health_(config, time_) {}
bool DiagnosticsManager::registerDevice(const std::string& deviceId) {
    std::lock_guard<std::mutex> lock(mutex_);
    return health_.registerDevice(deviceId);
}
void DiagnosticsManager::observe(const UnifiedMessageV2& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto transition = health_.observe(message)) alarms_.healthTransition(*transition);
}
void DiagnosticsManager::evaluate() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& transition : health_.evaluate()) alarms_.healthTransition(transition);
}
void DiagnosticsManager::reportQueueStall(bool stalled, const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    alarms_.setSystemAlarm(key, stalled, time_.epochMs());
}
bool DiagnosticsManager::acknowledgeAlarm(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    return alarms_.acknowledge(key, time_.epochMs());
}
std::vector<AlarmEvent> DiagnosticsManager::alarmEventsSince(std::uint64_t sequence) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return alarms_.eventsSince(sequence);
}
std::optional<HealthSnapshot> DiagnosticsManager::health(const std::string& deviceId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return health_.snapshot(deviceId);
}
void DiagnosticsManager::write(std::ostream& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    out << "{\"device_health\":";
    health_.appendMetrics(out);
    out << ",\"alarms\":";
    alarms_.appendMetrics(out);
    out << '}';
}
void DiagnosticsManager::writeDevice(std::ostream& out, const std::string& deviceId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream item;
    const bool found = health_.writeDevice(item, deviceId);
    out << "{\"found\":" << (found ? "true" : "false") << ",\"device\":";
    if (found) out << item.str();
    else out << "null";
    out << '}';
}
void DiagnosticsManager::appendMetrics(std::ostream& out) const {
    out << ",\"diagnostics\":";
    write(out);
}
}  // namespace mqmgateway::edge
