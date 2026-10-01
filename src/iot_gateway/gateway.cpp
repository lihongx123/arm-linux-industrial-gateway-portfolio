#include "gateway.hpp"

#include <mosquitto.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace mqmgateway::iot {

Gateway::Gateway(GatewayConfig config)
    : config_(std::move(config)), can_(config_.canInterface, config_.pipelineMetrics), queue_(config_.queueCapacity) {
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
}

Gateway::~Gateway() {
    stop();
}

void Gateway::start() {
    if (running_.exchange(true)) {
        return;
    }
    if (mosquitto_lib_init() != MOSQ_ERR_SUCCESS) {
        running_ = false;
        throw std::runtime_error("mosquitto_lib_init failed");
    }
    mqttLibraryInitialized_ = true;
    try {
    can_.open();
    southbound_ = std::make_unique<SouthboundReactor>(can_, config_.rtu, config_.queueCapacity,
        [this](UnifiedMessage message) {
            if (queue_.tryPush(std::move(message))) ++telemetryEnqueued_;
        });
    mqtt_ = mosquitto_new(config_.clientId.c_str(), true, this);
    if (mqtt_ == nullptr) {
        running_ = false;
        throw std::runtime_error("mosquitto_new failed");
    }
    // Public MQTT 3.x-compatible setter. Bounded window, never unlimited (0).
    const int windowResult = mosquitto_max_inflight_messages_set(mqtt_, config_.mqttMaxInflight);
    if (windowResult != MOSQ_ERR_SUCCESS) {
        throw std::runtime_error(std::string("MQTT inflight window: ") + mosquitto_strerror(windowResult));
    }
    if (!config_.mqttUsername.empty()) {
        const int authResult = mosquitto_username_pw_set(
            mqtt_, config_.mqttUsername.c_str(), config_.mqttPassword.c_str());
        if (authResult != MOSQ_ERR_SUCCESS) {
            throw std::runtime_error(std::string("MQTT authentication configuration: ") + mosquitto_strerror(authResult));
        }
    }
    if (config_.mqttTls) {
        const int tlsResult = mosquitto_tls_set(
            mqtt_, config_.mqttCaFile.c_str(), nullptr, nullptr, nullptr, nullptr);
        if (tlsResult != MOSQ_ERR_SUCCESS) {
            throw std::runtime_error(std::string("MQTT CA configuration: ") + mosquitto_strerror(tlsResult));
        }
        const int tlsOptionsResult = mosquitto_tls_opts_set(mqtt_, 1, "tlsv1.2", nullptr);
        if (tlsOptionsResult != MOSQ_ERR_SUCCESS) {
            throw std::runtime_error(std::string("MQTT TLS verification configuration: ") + mosquitto_strerror(tlsOptionsResult));
        }
    }
    mosquitto_connect_callback_set(mqtt_, &Gateway::connectedCallback);
    mosquitto_disconnect_callback_set(mqtt_, &Gateway::disconnectedCallback);
    mosquitto_message_callback_set(mqtt_, &Gateway::messageCallback);
    if (config_.pipelineMetrics) mosquitto_publish_callback_set(mqtt_, &Gateway::publishedCallback);
    mosquitto_reconnect_delay_set(mqtt_, 1, 30, true);
    const auto connectResult = mosquitto_connect_async(
        mqtt_, config_.mqttHost.c_str(), config_.mqttPort, config_.mqttKeepalive);
    if (connectResult != MOSQ_ERR_SUCCESS) {
        running_ = false;
        throw std::runtime_error(std::string("mosquitto_connect_async: ") + mosquitto_strerror(connectResult));
    }
    if (mosquitto_loop_start(mqtt_) != MOSQ_ERR_SUCCESS) {
        running_ = false;
        throw std::runtime_error("mosquitto_loop_start failed");
    }
    mqttLoopStarted_ = true;
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
    if (southbound_) southbound_->wake();
    if (receiver_.joinable()) receiver_.join();
    if (heartbeat_.joinable()) heartbeat_.join();
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
    workers_.clear();
    if (mqtt_ != nullptr) {
        const auto statusTopic = config_.cloudMode
            ? "resume/gateway/status/" + config_.clientId
            : "gateway/" + config_.clientId + "/status";
        if (connected_) publish(statusTopic, "{\"status\":\"offline\"}", true);
        mosquitto_disconnect(mqtt_);
        if (mqttLoopStarted_) mosquitto_loop_stop(mqtt_, true);
        mqttLoopStarted_ = false;
        mosquitto_destroy(mqtt_);
        mqtt_ = nullptr;
    }
    connected_ = false;
    writeMetrics();
    if (mqttLibraryInitialized_) {
        mosquitto_lib_cleanup();
        mqttLibraryInitialized_ = false;
    }
    can_.close();
}

void Gateway::wait() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void Gateway::connectedCallback(mosquitto*, void* context, const int result) {
    static_cast<Gateway*>(context)->onConnected(result);
}

void Gateway::disconnectedCallback(mosquitto*, void* context, const int result) {
    static_cast<Gateway*>(context)->onDisconnected(result);
}

void Gateway::messageCallback(mosquitto*, void* context, const mosquitto_message* message) {
    const std::string payload(static_cast<const char*>(message->payload), static_cast<std::size_t>(message->payloadlen));
    static_cast<Gateway*>(context)->onMessage(message->topic, payload);
}

void Gateway::publishedCallback(mosquitto*, void* context, int mid) {
    static_cast<Gateway*>(context)->publishTracker_.completed(mid);
}

void Gateway::onConnected(const int result) {
    std::cerr << "MQTT connected result=" << result << '\n';
    connected_ = result == 0;
    if (!connected_) {
        return;
    }
    const char* commandTopic = config_.cloudMode
        ? "resume/gateway/devices/+/command"
        : "device/+/cmd/+";
    mosquitto_subscribe(mqtt_, nullptr, commandTopic, 1);
    const auto statusTopic = config_.cloudMode
        ? "resume/gateway/status/" + config_.clientId
        : "gateway/" + config_.clientId + "/status";
    publish(statusTopic, "{\"status\":\"online\"}", true);
}

void Gateway::onDisconnected(const int result) {
    std::cerr << "MQTT disconnected result=" << result << '\n';
    connected_ = false;
}

void Gateway::onMessage(const std::string& topic, const std::string& payload) {
    std::string routedTopic = topic;
    if (config_.cloudMode) {
        constexpr char prefix[] = "resume/gateway/devices/";
        constexpr char suffix[] = "/command";
        const auto suffixPosition = topic.size() >= sizeof(suffix) - 1
            ? topic.size() - (sizeof(suffix) - 1)
            : std::string::npos;
        if (topic.rfind(prefix, 0) != 0 || suffixPosition == std::string::npos ||
            topic.compare(suffixPosition, sizeof(suffix) - 1, suffix) != 0 ||
            suffixPosition <= sizeof(prefix) - 1 ||
            topic.find('/', sizeof(prefix) - 1) != suffixPosition) {
            publishStatus("unknown", "rejected", "topic must match resume/gateway/devices/{id}/command");
            return;
        }
        const auto deviceId = topic.substr(sizeof(prefix) - 1, suffixPosition - (sizeof(prefix) - 1));
        routedTopic = "device/" + deviceId + "/cmd/" + (deviceId.rfind("rtu-", 0) == 0 ? "modbus_write" : "can_tx");
    }
    auto route = router_.route(routedTopic, payload);
    if (!route.accepted) {
        publishStatus(route.deviceId.empty() ? "unknown" : route.deviceId, "rejected", route.error);
        return;
    }
    if (!queue_.tryPush(std::move(route.message))) {
        publishStatus(route.deviceId, "rejected", "queue capacity exceeded");
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
            if (std::chrono::steady_clock::now() - item->enqueuedAt > item->timeout) {
                ++commandTimeouts_;
                publishStatus(item->deviceId, "timeout", "command expired in queue");
            } else if (item->protocol == Protocol::modbus_rtu) {
                const auto deviceId = item->deviceId;
                if (!southbound_->submit(std::move(*item)))
                    publishStatus(deviceId, "rejected", "RTU device unavailable or transaction queue full");
            } else if (can_.send(*item)) {
                publishStatus(item->deviceId, "ok", "CAN frame transmitted");
            } else {
                ++canErrors_;
                publishStatus(item->deviceId, "error", "CAN transmit failed");
            }
        } else if (item->dataType == DataType::status && item->protocol == Protocol::modbus_rtu) {
            publishStatus(item->deviceId, item->quality == Quality::good ? "ok" : toString(item->quality), "RTU transaction response");
        } else {
            const auto payload = toJson(*item);
            if (config_.pipelineMetrics) telemetryWork_.observe(std::chrono::steady_clock::now() - workerStart);
            publish(telemetryTopic(item->deviceId), payload, false, true);
        }
    }
}

void Gateway::heartbeatLoop() {
    while (running_) {
        const auto metrics = queue_.metrics();
        const std::string payload = "{\"status\":\"online\",\"queue_depth\":" +
            std::to_string(metrics.currentDepth) + ",\"queue_peak\":" +
            std::to_string(metrics.peakDepth) + "}";
        const auto heartbeatTopic = config_.cloudMode
            ? "resume/gateway/status/" + config_.clientId + "/heartbeat"
            : "gateway/" + config_.clientId + "/heartbeat";
        publish(heartbeatTopic, payload);
        writeMetrics();
        auto slept = std::chrono::milliseconds(0);
        while (running_ && slept < config_.heartbeatInterval) {
            constexpr auto quantum = std::chrono::milliseconds(50);
            std::this_thread::sleep_for(quantum);
            slept += quantum;
        }
    }
}

bool Gateway::publish(const std::string& topic, const std::string& payload, const bool retain, const bool telemetry) {
    if (mqtt_ == nullptr) {
        ++publishFailures_;
        return false;
    }
    for (int attempt = 0; attempt < 6; ++attempt) {
        if (connected_) {
            const int qos = telemetry ? config_.telemetryQos : 1;
            const auto send = [&](int* mid) {
                return mosquitto_publish(mqtt_, mid, topic.c_str(), static_cast<int>(payload.size()), payload.data(), qos, retain);
            };
            const auto result = config_.pipelineMetrics ? publishTracker_.submit(qos, telemetry, send) : send(nullptr);
            if (result == MOSQ_ERR_SUCCESS) {
                ++published_;
                return true;
            }
        }
        const auto backoff = std::min(1000, 100 * (1 << attempt));
        std::this_thread::sleep_for(std::chrono::milliseconds(backoff));
    }
    ++publishFailures_;
    return false;
}

void Gateway::publishStatus(const std::string& deviceId, const std::string& status, const std::string& detail) {
    const auto topic = config_.cloudMode
        ? "resume/gateway/devices/" + deviceId + "/command/result"
        : deviceStatusTopic(deviceId);
    publish(topic, "{\"status\":\"" + status + "\",\"detail\":\"" + detail + "\"}");
}

std::string Gateway::telemetryTopic(const std::string& deviceId) const {
    return config_.cloudMode
        ? "resume/gateway/devices/" + deviceId + "/telemetry"
        : "device/" + deviceId + "/telemetry";
}

std::string Gateway::deviceStatusTopic(const std::string& deviceId) const {
    return config_.cloudMode
        ? "resume/gateway/devices/" + deviceId + "/status"
        : "device/" + deviceId + "/status";
}

void Gateway::writeMetrics() const {
    if (config_.metricsFile.empty()) {
        return;
    }
    const auto metrics = queue_.metrics();
    const auto temporary = config_.metricsFile + ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    output << "{\"enqueued\":" << metrics.enqueued
           << ",\"dequeued\":" << metrics.dequeued
           << ",\"current_depth\":" << metrics.currentDepth
           << ",\"peak_depth\":" << metrics.peakDepth
           << ",\"rejected\":" << metrics.rejected
           << ",\"processing_latency_ms\":" << metrics.processingLatencyMs
           << ",\"published\":" << published_.load()
           << ",\"publish_failures\":" << publishFailures_.load()
           << ",\"command_timeouts\":" << commandTimeouts_.load()
           << ",\"can_errors\":" << canErrors_.load()
           << ",\"telemetry_qos\":" << config_.telemetryQos
           << ",\"mqtt_max_inflight\":" << config_.mqttMaxInflight
           << ",\"pipeline_metrics_enabled\":" << (config_.pipelineMetrics ? "true" : "false");
    if (config_.pipelineMetrics) {
        output << ",\"can_pipeline\":";
        can_.writeMetrics(output);
        output << ",\"telemetry_enqueued\":" << telemetryEnqueued_.load()
               << ",\"telemetry_dequeued\":" << telemetryDequeued_.load()
               << ",\"telemetry_queue_wait\":";
        telemetryQueueWait_.write(output);
        output << ",\"command_queue_wait\":";
        commandQueueWait_.write(output);
        output << ",\"telemetry_worker_before_publish\":";
        telemetryWork_.write(output);
        output << ",\"mqtt_pipeline\":";
        publishTracker_.write(output);
    }
    if (southbound_) { output << ",\"rtu\":"; southbound_->writeMetrics(output); }
    output << "}\n";
    output.close();
    if (!output || std::rename(temporary.c_str(), config_.metricsFile.c_str()) != 0) {
        std::cerr << "metrics snapshot write failed: " << config_.metricsFile << '\n';
    }
}

}  // namespace mqmgateway::iot
