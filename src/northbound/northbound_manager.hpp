#pragma once

#include "northbound_adapter.hpp"
#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <vector>

namespace mqmgateway::northbound {

enum class CommandState { accepted, rejected, succeeded, failed, timed_out };

class NorthboundManager {
public:
    using CommandHandler = std::function<std::string(edge::UnifiedMessageV2&)>;
    explicit NorthboundManager(std::size_t commandLimit = 4096);
    ~NorthboundManager();
    NorthboundManager(const NorthboundManager&) = delete;
    NorthboundManager& operator=(const NorthboundManager&) = delete;
    bool add(std::unique_ptr<INorthboundAdapter> adapter);
    bool start();
    void stop() noexcept;
    void setCommandHandler(CommandHandler handler);
    std::vector<std::pair<std::string, PublishResult>> publish(const edge::UnifiedMessageV2& message);
    void receiveCommand(const std::string& adapterId, edge::UnifiedMessageV2 command,
                        const std::string& parseError = {});
    bool completeCommand(const edge::UnifiedMessageV2& result);
    void expireCommands();
    void failPending(const std::string& reason);
    bool healthy() const;
    void writeMetrics(std::ostream& out) const;
private:
    struct Record {
        std::string adapterId;
        std::string deviceId;
        std::chrono::steady_clock::time_point deadline;
        bool finished{false};
        bool admissionPublished{false};
        std::optional<edge::UnifiedMessageV2> earlyResult;
    };
    struct CommandCounters {
        std::uint64_t duplicate{0}, timedOut{0}, results{0}, rejected{0};
    };
    void resultTo(const std::string& adapterId, const edge::UnifiedMessageV2& command,
                  CommandState state, const std::string& reason);
    INorthboundAdapter* findAdapter(const std::string& id) const;
    const std::size_t commandLimit_;
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<INorthboundAdapter>> adapters_;
    std::map<std::string, Record> commands_;
    std::map<std::string, CommandCounters> adapterCommands_;
    std::deque<std::string> order_;
    CommandHandler handler_;
    std::atomic<std::uint64_t> sequence_{0};
    std::atomic<std::uint64_t> duplicate_{0}, timedOut_{0}, results_{0}, rejected_{0}, resultPublishFailed_{0};
    bool started_{false};
};

}  // namespace mqmgateway::northbound
