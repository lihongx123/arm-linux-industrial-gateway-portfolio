#include "northbound_manager.hpp"

#include <algorithm>
#include <stdexcept>

namespace mqmgateway::northbound {
namespace {
const char* stateName(CommandState state) {
    switch (state) {
    case CommandState::accepted: return "accepted";
    case CommandState::rejected: return "rejected";
    case CommandState::succeeded: return "succeeded";
    case CommandState::failed: return "failed";
    case CommandState::timed_out: return "timeout";
    }
    return "failed";
}
std::string jsonEscape(const std::string& value) {
    std::string out;
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out += '\\';
        if (c >= 32) out += static_cast<char>(c);
    }
    return out;
}
}

NorthboundManager::NorthboundManager(std::size_t commandLimit) : commandLimit_(commandLimit) {
    if (!commandLimit_) throw std::invalid_argument("northbound command limit must be positive");
}
NorthboundManager::~NorthboundManager() { stop(); }
bool NorthboundManager::add(std::unique_ptr<INorthboundAdapter> adapter) {
    if (!adapter || adapter->id().empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_ || findAdapter(adapter->id())) return false;
    const auto id = adapter->id();
    adapter->setCommandHandler([this, id](edge::UnifiedMessageV2 command, std::string error) {
        receiveCommand(id, std::move(command), error);
    });
    adapters_.push_back(std::move(adapter));
    adapterCommands_.emplace(id, CommandCounters{});
    return true;
}
bool NorthboundManager::start() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (started_) return false;
        started_ = true;
    }
    bool all = true;
    for (const auto& adapter : adapters_) {
        try { if (!adapter->start()) all = false; }
        catch (...) { all = false; }
    }
    return all;
}
void NorthboundManager::stop() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) return;
        started_ = false;
    }
    failPending("northbound manager stopped before command completion");
    for (const auto& adapter : adapters_) adapter->stop();
}
void NorthboundManager::setCommandHandler(CommandHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    handler_ = std::move(handler);
}
std::vector<std::pair<std::string, PublishResult>> NorthboundManager::publish(const edge::UnifiedMessageV2& message) {
    std::vector<std::pair<std::string, PublishResult>> results;
    for (const auto& adapter : adapters_) {
        try { results.emplace_back(adapter->id(), adapter->publish(message)); }
        catch (const std::exception& error) { results.emplace_back(adapter->id(), PublishResult{false, error.what()}); }
        catch (...) { results.emplace_back(adapter->id(), PublishResult{false, "adapter exception"}); }
    }
    return results;
}
void NorthboundManager::resultTo(const std::string& adapterId, const edge::UnifiedMessageV2& command,
                                 CommandState state, const std::string& reason) {
    auto result = command;
    result.northboundType = edge::NorthboundType::command_result;
    result.dataType = iot::DataType::status;
    result.direction = iot::Direction::northbound;
    result.commandState = stateName(state);
    result.status = (state == CommandState::succeeded || state == CommandState::failed) &&
                    !command.status.empty() ? command.status : stateName(state);
    result.detail = reason;
    result.sourceTime = std::chrono::system_clock::now();
    result.quality = state == CommandState::timed_out ? iot::Quality::timeout :
                     (state == CommandState::accepted || state == CommandState::succeeded ?
                      iot::Quality::good : iot::Quality::unavailable);
    if (auto* adapter = findAdapter(adapterId)) {
        try { if (!adapter->publish(result).accepted) ++resultPublishFailed_; }
        catch (...) { ++resultPublishFailed_; }
    }
    ++results_;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& counters = adapterCommands_[adapterId];
        ++counters.results;
        if (state == CommandState::timed_out) ++counters.timedOut;
        if (state == CommandState::rejected) ++counters.rejected;
    }
}
INorthboundAdapter* NorthboundManager::findAdapter(const std::string& id) const {
    for (const auto& adapter : adapters_) if (adapter->id() == id) return adapter.get();
    return nullptr;
}
void NorthboundManager::receiveCommand(const std::string& adapterId, edge::UnifiedMessageV2 command,
                                       const std::string& parseError) {
    if (!findAdapter(adapterId)) return;
    if (command.deviceId.empty()) command.deviceId = "unknown";
    command.sourceAdapterId = adapterId;
    command.northboundType = edge::NorthboundType::command;
    command.dataType = iot::DataType::command;
    if (command.correlationId.empty())
        command.correlationId = adapterId + "-" + std::to_string(++sequence_);
    if (command.correlationId.size() > 128 || !parseError.empty()) {
        ++rejected_;
        resultTo(adapterId, command, CommandState::rejected,
                 !parseError.empty() ? parseError : "command_id exceeds 128 bytes");
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!command.deadline) command.deadline = now + command.timeout;
    if (*command.deadline <= now) {
        ++timedOut_;
        resultTo(adapterId, command, CommandState::timed_out, "deadline already expired");
        return;
    }
    CommandHandler handler;
    std::string error;
    bool inserted = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) error = "northbound manager is stopping";
        else if (commands_.find(command.correlationId) != commands_.end()) {
            ++duplicate_;
            ++adapterCommands_[adapterId].duplicate;
            error = "duplicate command_id";
        } else {
            while (commands_.size() >= commandLimit_) {
                const auto old = std::find_if(order_.begin(), order_.end(), [this](const std::string& id) {
                    const auto it = commands_.find(id);
                    return it != commands_.end() && it->second.finished;
                });
                if (old == order_.end()) break;
                commands_.erase(*old);
                order_.erase(old);
            }
            if (commands_.size() >= commandLimit_) error = "command tracking capacity exceeded";
            else {
                commands_.emplace(command.correlationId, Record{adapterId, command.deviceId, *command.deadline, false});
                order_.push_back(command.correlationId);
                handler = handler_;
                inserted = true;
            }
        }
    }
    if (error.empty()) {
        if (!handler) error = "command pipeline unavailable";
        else {
            try { error = handler(command); }
            catch (const std::exception& failure) { error = failure.what(); }
            catch (...) { error = "command pipeline exception"; }
        }
    }
    if (!error.empty()) {
        ++rejected_;
        if (inserted) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = commands_.find(command.correlationId);
            if (it != commands_.end() && it->second.adapterId == adapterId) it->second.finished = true;
        }
        resultTo(adapterId, command, CommandState::rejected, error);
        return;
    }
    resultTo(adapterId, command, CommandState::accepted, "queued for driver dispatch");
    std::optional<edge::UnifiedMessageV2> early;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = commands_.find(command.correlationId);
        if (it != commands_.end()) {
            it->second.admissionPublished = true;
            early = std::move(it->second.earlyResult);
        }
    }
    if (early) completeCommand(*early);
}
bool NorthboundManager::completeCommand(const edge::UnifiedMessageV2& result) {
    if (result.correlationId.empty()) return false;
    std::string adapterId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = commands_.find(result.correlationId);
        if (it == commands_.end() || it->second.finished) return false;
        if (!it->second.admissionPublished) {
            it->second.earlyResult = result;
            return true;
        }
        it->second.finished = true;
        adapterId = it->second.adapterId;
    }
    const auto state = result.status == "rejected" ? CommandState::rejected
        : result.quality == iot::Quality::timeout || result.status == "timeout"
        ? CommandState::timed_out
        : (result.quality == iot::Quality::good &&
           (result.status == "ok" || result.status == "success" || result.status == "succeeded"))
          ? CommandState::succeeded : CommandState::failed;
    if (state == CommandState::timed_out) ++timedOut_;
    if (state == CommandState::rejected) ++rejected_;
    resultTo(adapterId, result, state, result.detail);
    return true;
}
void NorthboundManager::expireCommands() {
    std::vector<edge::UnifiedMessageV2> expired;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        for (auto& entry : commands_) {
            if (entry.second.finished || entry.second.deadline > now) continue;
            edge::UnifiedMessageV2 result;
            result.deviceId = entry.second.deviceId;
            result.correlationId = entry.first;
            result.status = "timeout";
            result.detail = "command deadline expired";
            result.quality = iot::Quality::timeout;
            expired.push_back(std::move(result));
        }
    }
    for (const auto& entry : expired) completeCommand(entry);
}
void NorthboundManager::failPending(const std::string& reason) {
    std::vector<std::pair<std::string, edge::UnifiedMessageV2>> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : commands_) {
            if (entry.second.finished) continue;
            entry.second.finished = true;
            edge::UnifiedMessageV2 result;
            result.deviceId = entry.second.deviceId;
            result.correlationId = entry.first;
            pending.emplace_back(entry.second.adapterId, std::move(result));
        }
    }
    for (const auto& entry : pending) resultTo(entry.first, entry.second, CommandState::failed, reason);
}
bool NorthboundManager::healthy() const {
    if (adapters_.empty()) return false;
    return std::all_of(adapters_.begin(), adapters_.end(), [](const auto& adapter) { return adapter->healthy(); });
}
void NorthboundManager::writeMetrics(std::ostream& out) const {
    out << ",\"northbound\":{\"duplicate_commands\":" << duplicate_.load()
        << ",\"timed_out_commands\":" << timedOut_.load()
        << ",\"command_results\":" << results_.load()
        << ",\"command_result_publish_failed\":" << resultPublishFailed_.load()
        << ",\"rejected_commands\":" << rejected_.load() << ",\"adapters\":[";
    bool first = true;
    for (const auto& adapter : adapters_) {
        if (!first) out << ',';
        first = false;
        const auto metrics = adapter->metrics();
        CommandCounters counters;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = adapterCommands_.find(adapter->id());
            if (it != adapterCommands_.end()) counters = it->second;
        }
        out << "{\"id\":\"" << jsonEscape(adapter->id()) << "\",\"published\":" << metrics.published
            << ",\"publish_failed\":" << metrics.publishFailed
            << ",\"dropped\":" << metrics.dropped
            << ",\"inbound_commands\":" << metrics.inboundCommands
            << ",\"queue_depth\":" << metrics.queueDepth
            << ",\"queue_peak\":" << metrics.queuePeak
            << ",\"pending_publishes\":" << metrics.pendingPublishes
            << ",\"duplicate_commands\":" << counters.duplicate
            << ",\"timed_out_commands\":" << counters.timedOut
            << ",\"command_results\":" << counters.results
            << ",\"rejected_commands\":" << counters.rejected
            << ",\"healthy\":" << (metrics.healthy ? "true" : "false") << '}';
    }
    out << "]}";
}

}  // namespace mqmgateway::northbound
