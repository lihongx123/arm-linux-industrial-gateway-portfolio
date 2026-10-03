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
      telemetryQueue_(config_.queueCapacity),
      commandQueue_(config_.commandQueueCapacity),
      core_([this](edge::UnifiedMessageV2 message) {
          if (!message.correlationId.empty() && message.dataType == DataType::status &&
              northbound_.completeCommand(message)) return;
          if (message.dataType == DataType::telemetry) {
              const auto point = core_.pointDefinition(message.deviceId, message.pointId);
              const auto decision = telemetryPolicy_.process(message,
                  point ? point->type : edge::PointValueType::bytes,
                  [this, &message] { return telemetryQueue_.tryPush(message); });
              if (decision == edge::TelemetryPolicy::Decision::published) ++telemetryEnqueued_;
          } else if (telemetryQueue_.tryPush(std::move(message))) ++telemetryEnqueued_;
      }, config_.health),
      telemetryQueueWatchdog_(config_.queueWatchdogThreshold),
      commandQueueWatchdog_(config_.queueWatchdogThreshold) {
    drivers::registerSouthboundDrivers(core_, config_.canInterface, config_.pipelineMetrics, config_.rtu, config_.queueCapacity);
    drivers::registerTcpDrivers(core_, config_.modbusTcp, config_.genericTcp, config_.queueCapacity);
    drivers::registerMcDrivers(core_, config_.mc, config_.queueCapacity);
    drivers::registerOpcUaDrivers(core_, config_.opcua);
    drivers::registerS7Drivers(core_, config_.s7);
    drivers::registerBoardDrivers(core_, config_.spi, config_.i2c, config_.gpio, config_.adc, config_.pwm);
    drivers::registerUartDrivers(core_, config_.uart, config_.queueCapacity);
    if (!config_.queueCapacity || !config_.workers || !config_.commandQueueCapacity || !config_.commandWorkers)
        throw std::invalid_argument("telemetry/command queue capacities and workers must be positive");
    for (const auto& entry : config_.pointPolicies)
        if (!telemetryPolicy_.configure(entry.deviceId, entry.pointId, entry.policy))
            throw std::invalid_argument("invalid or excessive point policy");
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
    mqtt.outboundCapacity = config_.mqttOutboundCapacity ? config_.mqttOutboundCapacity : config_.queueCapacity;
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
        if (!commandQueue_.tryPush(command)) return "command queue capacity exceeded";
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
        workers_.emplace_back(&Gateway::telemetryWorkerLoop, this);
    }
    for (std::size_t index = 0; index < config_.commandWorkers; ++index)
        commandWorkers_.emplace_back(&Gateway::commandWorkerLoop, this);
    heartbeat_ = std::thread(&Gateway::heartbeatLoop, this);
    } catch (...) {
        stop();
        throw;
    }
}

void Gateway::stop() {
    running_ = false;
    telemetryQueue_.stop();
    commandQueue_.stop();
    if (acquisition_) acquisition_->stop();
    if (southbound_) southbound_->wake();
    if (receiver_.joinable()) receiver_.join();
    if (heartbeat_.joinable()) heartbeat_.join();
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
    workers_.clear();
    for (auto& worker : commandWorkers_) if (worker.joinable()) worker.join();
    commandWorkers_.clear();
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
        telemetryQueue_.stop();
        commandQueue_.stop();
    }
}

void Gateway::telemetryWorkerLoop() {
    while (running_) {
        auto item = telemetryQueue_.pop();
        if (!item) {
            return;
        }
        const auto workerStart = std::chrono::steady_clock::now();
        if (config_.pipelineMetrics) {
            ++telemetryDequeued_;
            telemetryQueueWait_.observe(workerStart - item->enqueuedAt);
        }
        const auto latency = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - item->enqueuedAt).count();
        telemetryQueue_.observeLatency(latency);
        if (config_.processingDelay.count() > 0) {
            std::this_thread::sleep_for(config_.processingDelay);
        }
        if (item->dataType == DataType::status) {
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
void Gateway::commandWorkerLoop() {
    while (running_) {
        auto item = commandQueue_.pop();
        if (!item) return;
        const auto now = std::chrono::steady_clock::now();
        if (config_.pipelineMetrics) commandQueueWait_.observe(now - item->enqueuedAt);
        commandQueue_.observeLatency(std::chrono::duration<double, std::milli>(now - item->enqueuedAt).count());
        if (config_.processingDelay.count() > 0) std::this_thread::sleep_for(config_.processingDelay);
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
    }
}

void Gateway::heartbeatLoop() {
    while (running_) {
        const auto metrics = telemetryQueue_.metrics();
        const auto commands = commandQueue_.metrics();
        const auto watchdog = telemetryQueueWatchdog_.observe(
            metrics.currentDepth, metrics.dequeued, std::chrono::steady_clock::now());
        if (watchdog) {
            const bool stalled = *watchdog == edge::QueueWatchdog::Transition::stalled;
            core_.reportQueueStall(stalled, "telemetry_queue_stalled");
            core_.reportQueueStall(stalled, "queue_stalled"); // legacy Phase 6 alarm key
            std::cerr << "telemetry queue watchdog " << (stalled ? "stalled" : "recovered") << '\n';
        }
        const auto commandWatchdog = commandQueueWatchdog_.observe(
            commands.currentDepth, commands.dequeued, std::chrono::steady_clock::now());
        if (commandWatchdog) {
            const bool stalled = *commandWatchdog == edge::QueueWatchdog::Transition::stalled;
            core_.reportQueueStall(stalled, "command_queue_stalled");
            std::cerr << "command queue watchdog " << (stalled ? "stalled" : "recovered") << '\n';
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
    const auto metrics = telemetryQueue_.metrics();
    const auto commands = commandQueue_.metrics();
    const auto policy = telemetryPolicy_.metrics();
    const auto temporary = config_.metricsFile + ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    const auto mqttMetrics = mqttAdapter_->metrics();
    output << "{\"enqueued\":" << metrics.enqueued
           << ",\"dequeued\":" << metrics.dequeued
           << ",\"current_depth\":" << metrics.currentDepth
           << ",\"peak_depth\":" << metrics.peakDepth
           << ",\"rejected\":" << metrics.rejected
           << ",\"processing_latency_ms\":" << metrics.processingLatencyMs
           << ",\"telemetry_samples_valid\":" << policy.valid
           << ",\"telemetry_published\":" << policy.published
           << ",\"telemetry_suppressed_cov\":" << policy.suppressedCov
           << ",\"telemetry_forced_max_interval\":" << policy.forcedMaxInterval
           << ",\"telemetry_mapping_failed\":" << core_.mappingFailures()
           << ",\"telemetry_queue_rejected\":" << metrics.rejected
           << ",\"command_queue_rejected\":" << commands.rejected
           << ",\"telemetry_queue\":{\"enqueued\":" << metrics.enqueued
           << ",\"dequeued\":" << metrics.dequeued
           << ",\"current_depth\":" << metrics.currentDepth
           << ",\"peak_depth\":" << metrics.peakDepth
           << ",\"rejected\":" << metrics.rejected
           << ",\"processing_latency_ms\":" << metrics.processingLatencyMs << '}'
           << ",\"command_queue\":{\"enqueued\":" << commands.enqueued
           << ",\"dequeued\":" << commands.dequeued
           << ",\"current_depth\":" << commands.currentDepth
           << ",\"peak_depth\":" << commands.peakDepth
           << ",\"rejected\":" << commands.rejected
           << ",\"processing_latency_ms\":" << commands.processingLatencyMs << '}'
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
    telemetryQueueWatchdog_.appendMetrics(output); // legacy Phase 6 metric
    telemetryQueueWatchdog_.appendMetrics(output, "telemetry_queue_watchdog");
    commandQueueWatchdog_.appendMetrics(output, "command_queue_watchdog");
    if (acquisition_) acquisition_->appendMetrics(output);
    output << "}\n";
    output.close();
    if (!output || std::rename(temporary.c_str(), config_.metricsFile.c_str()) != 0) {
        std::cerr << "metrics snapshot write failed: " << config_.metricsFile << '\n';
    }
}

}  // namespace mqmgateway::iot
