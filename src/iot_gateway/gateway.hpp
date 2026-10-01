#pragma once

#include "bounded_queue.hpp"
#include "can_socket.hpp"
#include "command_router.hpp"
#include "southbound_reactor.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct mosquitto;
struct mosquitto_message;

namespace mqmgateway::iot {

struct GatewayConfig {
    std::string mqttHost{"127.0.0.1"};
    int mqttPort{18883};
    int mqttKeepalive{10};
    std::string mqttUsername;
    std::string mqttPassword;
    std::string mqttCaFile;
    bool mqttTls{false};
    bool cloudMode{false};
    std::string clientId{"mqmgateway-iot"};
    std::string canInterface{"vcan0"};
    std::size_t queueCapacity{1024};
    std::size_t workers{2};
    std::chrono::milliseconds heartbeatInterval{1000};
    std::chrono::milliseconds processingDelay{0};
    std::string metricsFile;
    bool pipelineMetrics{false};
    int telemetryQos{1};
    unsigned int mqttMaxInflight{20};
    RtuConfig rtu;
};

class Gateway {
public:
    explicit Gateway(GatewayConfig config);
    ~Gateway();
    Gateway(const Gateway&) = delete;
    Gateway& operator=(const Gateway&) = delete;

    void start();
    void stop();
    void wait();
    bool isRunning() const { return running_.load(); }

private:
    static void connectedCallback(mosquitto* client, void* context, int result);
    static void disconnectedCallback(mosquitto* client, void* context, int result);
    static void messageCallback(mosquitto* client, void* context, const mosquitto_message* message);
    static void publishedCallback(mosquitto* client, void* context, int mid);

    void onConnected(int result);
    void onDisconnected(int result);
    void onMessage(const std::string& topic, const std::string& payload);
    void receiveLoop();
    void workerLoop();
    void heartbeatLoop();
    bool publish(const std::string& topic, const std::string& payload, bool retain = false, bool telemetry = false);
    void publishStatus(const std::string& deviceId, const std::string& status, const std::string& detail);
    std::string telemetryTopic(const std::string& deviceId) const;
    std::string deviceStatusTopic(const std::string& deviceId) const;
    void writeMetrics() const;

    GatewayConfig config_;
    CanSocket can_;
    CommandRouter router_;
    BoundedQueue<UnifiedMessage> queue_;
    std::unique_ptr<SouthboundReactor> southbound_;
    mosquitto* mqtt_{nullptr};
    bool mqttLibraryInitialized_{false}, mqttLoopStarted_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<std::uint64_t> published_{0};
    std::atomic<std::uint64_t> publishFailures_{0};
    std::atomic<std::uint64_t> commandTimeouts_{0};
    std::atomic<std::uint64_t> canErrors_{0};
    std::atomic<std::uint64_t> telemetryEnqueued_{0}, telemetryDequeued_{0};
    PublishTracker publishTracker_;
    StageLatency telemetryQueueWait_, commandQueueWait_, telemetryWork_;
    std::thread receiver_;
    std::thread heartbeat_;
    std::vector<std::thread> workers_;
};

}  // namespace mqmgateway::iot
