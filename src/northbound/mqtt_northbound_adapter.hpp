#pragma once

#include "northbound_adapter.hpp"
#include "command_router.hpp"
#include "pipeline_metrics.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

struct mosquitto;
struct mosquitto_message;

namespace mqmgateway::northbound {

struct MqttAdapterConfig {
    std::string id{"mqtt"};
    std::string host{"127.0.0.1"};
    int port{18883};
    int keepalive{10};
    std::string username, password, caFile, clientId{"mqmgateway-iot"};
    bool tls{false}, cloudMode{false}, diagnostics{false}, pipelineMetrics{false};
    int telemetryQos{1};
    unsigned maxInflight{20};
    std::size_t outboundCapacity{1024};
    std::function<std::string(const std::string&)> resolveCommand;
    std::function<bool(const std::string&)> acknowledgeAlarm;
};

class MqttNorthboundAdapter final : public INorthboundAdapter {
public:
    struct ParsedCommand {
        edge::UnifiedMessageV2 message;
        std::string error;
    };
    explicit MqttNorthboundAdapter(MqttAdapterConfig config);
    ~MqttNorthboundAdapter() override;
    std::string id() const override { return config_.id; }
    bool start() override;
    void stop() noexcept override;
    bool healthy() const override { return connected_.load(); }
    PublishResult publish(const edge::UnifiedMessageV2& message) override;
    void setCommandHandler(CommandHandler handler) override { commandHandler_ = std::move(handler); }
    AdapterMetrics metrics() const override;
    void writePipelineMetrics(std::ostream& out) const;
    std::string topicFor(const edge::UnifiedMessageV2& message) const;
    std::string serialize(const edge::UnifiedMessageV2& message) const;
    ParsedCommand parseCommand(const std::string& topic, const std::string& payload) const;
private:
    static void connectedCallback(mosquitto*, void*, int);
    static void disconnectedCallback(mosquitto*, void*, int);
    static void messageCallback(mosquitto*, void*, const mosquitto_message*);
    static void publishedCallback(mosquitto*, void*, int);
    void onConnected(int result);
    void onPublished(int mid);
    void onMessage(const std::string& topic, const std::string& payload);
    PublishResult publishRaw(const std::string& topic, const std::string& payload,
                             bool retain, bool telemetry);
    PublishResult queueRaw(std::string topic, std::string payload, bool retain, bool telemetry,
                           bool routine = false);
    void senderLoop();
    std::string gatewayStatusTopic() const;
    std::string diagnosticsTopic() const;
    MqttAdapterConfig config_;
    iot::CommandRouter router_;
    CommandHandler commandHandler_;
    mosquitto* mqtt_{nullptr};
    bool libraryInitialized_{false}, loopStarted_{false};
    std::atomic<bool> connected_{false};
    std::atomic<std::uint64_t> published_{0}, failed_{0}, dropped_{0}, inbound_{0};
    mutable std::mutex clientMutex_;
    std::unordered_set<int> pendingMids_;
    struct Outbound { std::string topic, payload; bool retain{false}, telemetry{false}; };
    mutable std::mutex outboundMutex_;
    std::condition_variable outboundReady_;
    std::deque<Outbound> telemetryOutbound_, controlOutbound_;
    std::size_t outboundPeak_{0};
    unsigned consecutiveControl_{0};
    bool senderRunning_{false};
    bool outboundInFlight_{false};
    std::thread sender_;
    iot::PublishTracker publishTracker_;
};

}  // namespace mqmgateway::northbound
