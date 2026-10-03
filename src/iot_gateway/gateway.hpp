#pragma once

#include "bounded_queue.hpp"
#include "pipeline_metrics.hpp"
#include "register_drivers.hpp"
#include "gateway_core.hpp"
#include "acquisition_scheduler.hpp"
#include "watchdog.hpp"
#include "telemetry_policy.hpp"
#include "southbound_reactor.hpp"
#include "mqtt_northbound_adapter.hpp"
#include "northbound_manager.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
    std::size_t commandQueueCapacity{64};
    std::size_t mqttOutboundCapacity{0}; // 0 preserves legacy alias to queueCapacity
    std::size_t workers{2};
    std::size_t commandWorkers{1};
    struct PointPolicyEntry {
        std::string deviceId, pointId;
        edge::ReportingPolicy policy;
    };
    std::vector<PointPolicyEntry> pointPolicies;
    std::chrono::milliseconds heartbeatInterval{1000};
    std::chrono::milliseconds processingDelay{0};
    std::string metricsFile;
    bool pipelineMetrics{false};
    bool diagnosticsMqtt{false};
    int telemetryQos{1};
    unsigned int mqttMaxInflight{20};
    edge::HealthConfig health;
    std::chrono::milliseconds queueWatchdogThreshold{5000};
    drivers::RtuConfig rtu;
    std::vector<drivers::ModbusTcpConfig> modbusTcp;
    std::vector<drivers::TcpConfig> genericTcp;
    std::vector<drivers::McConfig> mc;
    std::vector<drivers::OpcUaConfig> opcua;
    std::vector<drivers::S7Config> s7;
    std::vector<board::SpiConfig> spi;
    std::vector<board::I2cConfig> i2c;
    std::vector<board::GpioConfig> gpio;
    std::vector<board::AdcConfig> adc;
    std::vector<board::PwmConfig> pwm;
    std::vector<drivers::RawUartConfig> uart;
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
    void receiveLoop();
    void telemetryWorkerLoop();
    void commandWorkerLoop();
    void heartbeatLoop();
    void writeMetrics() const;

    GatewayConfig config_;
    BoundedQueue<edge::UnifiedMessageV2> telemetryQueue_;
    BoundedQueue<edge::UnifiedMessageV2> commandQueue_;
    edge::TelemetryPolicy telemetryPolicy_;
    edge::GatewayCore core_;
    northbound::NorthboundManager northbound_;
    northbound::MqttNorthboundAdapter* mqttAdapter_{nullptr}; // owned by northbound_
    std::unique_ptr<SouthboundReactor> southbound_;
    std::unique_ptr<edge::AcquisitionScheduler> acquisition_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> commandTimeouts_{0};
    std::atomic<std::uint64_t> telemetryEnqueued_{0}, telemetryDequeued_{0};
    std::uint64_t lastAlarmSequence_{0}; // heartbeat thread only
    std::atomic<std::uint64_t> alarmHistoryMissed_{0};
    StageLatency telemetryQueueWait_, commandQueueWait_, telemetryWork_;
    edge::QueueWatchdog telemetryQueueWatchdog_, commandQueueWatchdog_;
    std::thread receiver_;
    std::thread heartbeat_;
    std::vector<std::thread> workers_;
    std::vector<std::thread> commandWorkers_;
};

}  // namespace mqmgateway::iot
