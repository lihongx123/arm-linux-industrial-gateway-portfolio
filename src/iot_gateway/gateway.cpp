#include "gateway.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace mqmgateway::iot {

Gateway::Gateway(GatewayConfig config)
    : config_(std::move(config)),
      queue_(config_.queueCapacity),
      core_([this](edge::UnifiedMessageV2 message) {
          if (!message.correlationId.empty() && message.dataType == DataType::status &&
              northbound_.completeCommand(message)) return;
          if (queue_.tryPush(std::move(message))) ++telemetryEnqueued_;
      }, config_.health),
      queueWatchdog_(config_.queueWatchdogThreshold) {
    drivers::registerSouthboundDrivers(core_, config_.canInterface, config_.pipelineMetrics, config_.rtu, config_.queueCapacity);
    drivers::registerTcpDrivers(core_, config_.modbusTcp, config_.genericTcp, config_.queueCapacity);
    drivers::registerMcDrivers(core_, config_.mc, config_.queueCapacity);
    drivers::registerOpcUaDrivers(core_, config_.opcua);
    drivers::registerS7Drivers(core_, config_.s7);
    drivers::registerBoardDrivers(core_, config_.spi, config_.i2c, config_.gpio);
    drivers::registerUartDrivers(core_, config_.uart, config_.queueCapacity);
    if (!config_.queueCapacity || !config_.workers) throw std::invalid_argument("queue/workers must be positive");
    if (config_.cloudMode) {
        config_.mqttTls = true;
    }
    if (config_.telemetryQos != 0 && config_.telemetryQos != 1) {
        throw std::invalid_argument("telemetry QoS must be 0 or 1");
    }
    if (config_.mqttMaxInflight == 0 || config_.mqttMaxInflight > 65535) {
        throw std::invalid_argument("MQTT inflight window must be 1..65535");
    }
    if (config_.cloudMode && (config_.mqttHost == "127.0.0.1" || config_.mqttUsername.empty())) {
        throw std::invalid_argument("cloud mode requires EMQX host and gateway credentials");
    }
    if (config_.mqttTls && config_.mqttCaFile.empty()) {
        throw std::invalid_argument("MQTT TLS requires a CA certificate path");
    }
    if (config_.mqttUsername.empty() != config_.mqttPassword.empty()) {
        throw std::invalid_argument("MQTT username and password must be configured together");
    }
    northbound::MqttAdapterConfig mqtt;
    mqtt.host = config_.mqttHost;
    mqtt.port = config_.mqttPort;
    mqtt.keepalive = config_.mqttKeepalive;
    mqtt.username = config_.mqttUsername;
    mqtt.password = config_.mqttPassword;
    mqtt.caFile = config_.mqttCaFile;
    mqtt.tls = config_.mqttTls;
    mqtt.cloudMode = config_.cloudMode;
    mqtt.clientId = config_.clientId;
    mqtt.telemetryQos = config_.telemetryQos;
    mqtt.maxInflight = config_.mqttMaxInflight;
    mqtt.outboundCapacity = config_.queueCapacity;
    mqtt.pipelineMetrics = config_.pipelineMetrics;
    mqtt.diagnostics = config_.diagnosticsMqtt;
    mqtt.resolveCommand = [this](const std::string& id) { return core_.defaultCommand(id); };
    mqtt.acknowledgeAlarm = [this](const std::string& key) { return core_.acknowledgeAlarm(key); };
    auto adapter = std::make_unique<northbound::MqttNorthboundAdapter>(std::move(mqtt));
    mqttAdapter_ = adapter.get();
    if (!northbound_.add(std::move(adapter))) throw std::runtime_error("MQTT adapter registration failed");
    northbound_.setCommandHandler([this](edge::UnifiedMessageV2& command) -> std::string {
        if (core_.defaultCommand(command.deviceId) != command.operation)
            return "command does not match registered driver";
        if (!core_.prepare(command)) return "device has no matching registered driver";
        command.enqueuedAt = std::chrono::steady_clock::now();
        if (!queue_.tryPush(command)) return "queue capacity exceeded";
        return {};
    });
}

Gateway::~Gateway() {
    stop();
}

void Gateway::start() {
    if (running_.exchange(true)) {
        return;
    }
    try {
    core_.start();
    southbound_ = std::make_unique<SouthboundReactor>(core_.eventDrivers());
    acquisition_ = std::make_unique<edge::AcquisitionScheduler>(core_.acquisitionDrivers());
    acquisition_->start();
    if (!northbound_.start()) throw std::runtime_error("northbound adapter start failed");
    receiver_ = std::thread(&Gateway::receiveLoop, this);
    for (std::size_t index = 0; index < std::max<std::size_t>(1, config_.workers); ++index) {
        workers_.emplace_back(&Gateway::workerLoop, this);
    }
    heartbeat_ = std::thread(&Gateway::heartbeatLoop, this);
    } catch (...) {
        stop();
        throw;
    }
}

void Gateway::stop() {
    running_ = false;
    queue_.stop();
    if (acquisition_) acquisition_->stop();
    if (southbound_) southbound_->wake();
    if (receiver_.joinable()) receiver_.join();
    if (heartbeat_.joinable()) heartbeat_.join();
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
    workers_.clear();
    northbound_.stop();
    writeMetrics();
    core_.stop();
    acquisition_.reset();
    southbound_.reset();
}

void Gateway::wait() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void Gateway::receiveLoop() {
    try {
        southbound_->run(running_);
    } catch (const std::exception& error) {
        std::cerr << "southbound reactor failed: " << error.what() << '\n';
        running_ = false;
        queue_.stop();
    }
}

void Gateway::workerLoop() {
    while (running_) {
        auto item = queue_.pop();
        if (!item) {
            return;
        }
        const auto workerStart = std::chrono::steady_clock::now();
        if (config_.pipelineMetrics) {
            if (item->dataType == DataType::command) {
                commandQueueWait_.observe(workerStart - item->enqueuedAt);
            } else {
                ++telemetryDequeued_;
                telemetryQueueWait_.observe(workerStart - item->enqueuedAt);
            }
        }
        const auto latency = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - item->enqueuedAt).count();
        queue_.observeLatency(latency);
        if (config_.processingDelay.count() > 0) {
            std::this_thread::sleep_for(config_.processingDelay);
        }
        if (item->dataType == DataType::command) {
            if ((item->deadline && std::chrono::steady_clock::now() >= *item->deadline) ||
                std::chrono::steady_clock::now() - item->enqueuedAt > item->timeout) {
                ++commandTimeouts_;
                item->status = "timeout";
                item->detail = "command expired in queue";
                item->quality = Quality::timeout;
                northbound_.completeCommand(*item);
            } else if (!core_.submit(*item)) {
                item->status = "rejected";
                item->detail = "device unavailable or command rejected";
                item->quality = Quality::unavailable;
                northbound_.completeCommand(*item);
            }
        } else if (item->dataType == DataType::status) {
            item->northboundType = edge::NorthboundType::status;
            if (item->status.empty()) item->status = item->quality == Quality::good ? "ok" : toString(item->quality);
            northbound_.publish(*item);
        } else {
            item->northboundType = edge::NorthboundType::telemetry;
            if (config_.pipelineMetrics) telemetryWork_.observe(std::chrono::steady_clock::now() - workerStart);
            northbound_.publish(*item);
        }
    }
}

void Gateway::heartbeatLoop() {
    while (running_) {
        const auto metrics = queue_.metrics();
        const auto watchdog = queueWatchdog_.observe(
            metrics.currentDepth, metrics.dequeued, std::chrono::steady_clock::now());
        if (watchdog) {
            const bool stalled = *watchdog == edge::QueueWatchdog::Transition::stalled;
            core_.reportQueueStall(stalled);
            std::cerr << "queue watchdog " << (stalled ? "stalled" : "recovered") << '\n';
        }
        core_.evaluateDiagnostics();
        northbound_.expireCommands();
        if (config_.diagnosticsMqtt && northbound_.healthy()) {
            for (const auto& event : core_.alarmEventsSince(lastAlarmSequence_)) {
                edge::UnifiedMessageV2 alarm;
                alarm.northboundType = edge::NorthboundType::alarm;
                alarm.sourceDescriptor = "gateway";
                alarm.pointId = event.key;
                alarm.operation = event.action;
                alarm.status = event.severity;
                alarm.eventSequence = event.sequence;
                alarm.eventEpochMs = event.epochMs;
                const auto results = northbound_.publish(alarm);
                if (results.empty() || std::any_of(results.begin(), results.end(),
                    [](const auto& result) { return !result.second.accepted; })) break;
                if (event.sequence > lastAlarmSequence_ + 1)
                    alarmHistoryMissed_ += event.sequence - lastAlarmSequence_ - 1;
                lastAlarmSequence_ = event.sequence;
            }
            std::ostringstream snapshot;
            core_.writeDiagnostics(snapshot);
            edge::UnifiedMessageV2 diagnostic;
            diagnostic.northboundType = edge::NorthboundType::diagnostic;
            diagnostic.sourceDescriptor = "gateway";
            diagnostic.operation = "legacy_snapshot";
            diagnostic.cookedValue = snapshot.str();
            northbound_.publish(diagnostic);
        }
        edge::UnifiedMessageV2 heartbeat;
        heartbeat.northboundType = edge::NorthboundType::status;
        heartbeat.sourceDescriptor = "gateway";
        heartbeat.operation = "heartbeat";
        heartbeat.status = "online";
        heartbeat.rawValue = std::to_string(metrics.currentDepth);
        heartbeat.cookedValue = std::to_string(metrics.peakDepth);
        northbound_.publish(heartbeat);
        writeMetrics();
        auto slept = std::chrono::milliseconds(0);
        while (running_ && slept < config_.heartbeatInterval) {
            constexpr auto quantum = std::chrono::milliseconds(50);
            std::this_thread::sleep_for(quantum);
            slept += quantum;
        }
    }
}

void Gateway::writeMetrics() const {
    if (config_.metricsFile.empty()) {
        return;
    }
    const auto metrics = queue_.metrics();
    const auto temporary = config_.metricsFile + ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    const auto mqttMetrics = mqttAdapter_->metrics();
    output << "{\"enqueued\":" << metrics.enqueued
           << ",\"dequeued\":" << metrics.dequeued
           << ",\"current_depth\":" << metrics.currentDepth
           << ",\"peak_depth\":" << metrics.peakDepth
           << ",\"rejected\":" << metrics.rejected
           << ",\"processing_latency_ms\":" << metrics.processingLatencyMs
           << ",\"published\":" << mqttMetrics.published
           << ",\"publish_failures\":" << mqttMetrics.publishFailed
           << ",\"command_timeouts\":" << commandTimeouts_.load()
           << ",\"telemetry_qos\":" << config_.telemetryQos
           << ",\"mqtt_max_inflight\":" << config_.mqttMaxInflight
           << ",\"alarm_history_missed\":" << alarmHistoryMissed_.load()
           << ",\"pipeline_metrics_enabled\":" << (config_.pipelineMetrics ? "true" : "false");
    if (config_.pipelineMetrics) {
        output << ",\"telemetry_enqueued\":" << telemetryEnqueued_.load()
               << ",\"telemetry_dequeued\":" << telemetryDequeued_.load()
               << ",\"telemetry_queue_wait\":";
        telemetryQueueWait_.write(output);
        output << ",\"command_queue_wait\":";
        commandQueueWait_.write(output);
        output << ",\"telemetry_worker_before_publish\":";
        telemetryWork_.write(output);
        output << ",\"mqtt_pipeline\":";
        mqttAdapter_->writePipelineMetrics(output);
    }
    northbound_.writeMetrics(output);
    core_.appendMetrics(output);
    queueWatchdog_.appendMetrics(output);
    if (acquisition_) acquisition_->appendMetrics(output);
    output << "}\n";
    output.close();
    if (!output || std::rename(temporary.c_str(), config_.metricsFile.c_str()) != 0) {
        std::cerr << "metrics snapshot write failed: " << config_.metricsFile << '\n';
    }
}

}  // namespace mqmgateway::iot
