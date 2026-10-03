#include "mqtt_northbound_adapter.hpp"
#include "diagnostic_json.hpp"

#include <mosquitto.h>
#include <rapidjson/document.h>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace mqmgateway::northbound {
namespace {
std::mutex libraryMutex;
unsigned libraryUsers = 0;
std::string escapeJson(const std::string& input) {
    std::string out;
    for (unsigned char c : input) {
        if (c == '"' || c == '\\') out += '\\';
        if (c < 32) return {};
        out += static_cast<char>(c);
    }
    return out;
}
std::string telemetryJson(const edge::UnifiedMessageV2& message) {
    auto body = iot::toJson(edge::toLegacy(message));
    if (message.pointId.empty()) return body;
    body.pop_back();
    body += ",\"point_id\":\"" + escapeJson(message.pointId) +
            "\",\"raw_value\":\"" + escapeJson(message.rawValue) +
            "\",\"value\":\"" + escapeJson(message.cookedValue) +
            "\",\"driver_id\":\"" + escapeJson(message.driverId) + "\"";
    if (!message.sourceDescriptor.empty())
        body += ",\"source_descriptor\":\"" + escapeJson(message.sourceDescriptor) + "\"";
    if (message.protocolStatusCode) body += ",\"protocol_status_code\":" + std::to_string(*message.protocolStatusCode);
    if (message.sourceTimestamp)
        body += ",\"source_timestamp_ms\":" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
            message.sourceTimestamp->time_since_epoch()).count());
    if (message.serverTimestamp)
        body += ",\"server_timestamp_ms\":" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
            message.serverTimestamp->time_since_epoch()).count());
    body += '}';
    return body;
}
}

MqttNorthboundAdapter::MqttNorthboundAdapter(MqttAdapterConfig config) : config_(std::move(config)) {
    if (config_.id.empty() || config_.clientId.empty() || config_.host.empty() || config_.port <= 0 ||
        config_.telemetryQos < 0 || config_.telemetryQos > 1 || !config_.maxInflight || !config_.outboundCapacity)
        throw std::invalid_argument("invalid MQTT adapter configuration");
}
MqttNorthboundAdapter::~MqttNorthboundAdapter() { stop(); }
bool MqttNorthboundAdapter::start() {
    if (mqtt_) return false;
    {
        std::lock_guard<std::mutex> lock(libraryMutex);
        if (!libraryUsers && mosquitto_lib_init() != MOSQ_ERR_SUCCESS) return false;
        ++libraryUsers;
        libraryInitialized_ = true;
    }
    try {
        mqtt_ = mosquitto_new(config_.clientId.c_str(), true, this);
        if (!mqtt_) throw std::runtime_error("mosquitto_new failed");
        auto check = [](int code, const char* action) {
            if (code != MOSQ_ERR_SUCCESS)
                throw std::runtime_error(std::string(action) + ": " + mosquitto_strerror(code));
        };
        check(mosquitto_max_inflight_messages_set(mqtt_, config_.maxInflight), "MQTT inflight window");
        if (!config_.username.empty())
            check(mosquitto_username_pw_set(mqtt_, config_.username.c_str(), config_.password.c_str()), "MQTT authentication");
        if (config_.tls) {
            check(mosquitto_tls_set(mqtt_, config_.caFile.c_str(), nullptr, nullptr, nullptr, nullptr), "MQTT CA");
            check(mosquitto_tls_opts_set(mqtt_, 1, "tlsv1.2", nullptr), "MQTT TLS verification");
        }
        mosquitto_connect_callback_set(mqtt_, &MqttNorthboundAdapter::connectedCallback);
        mosquitto_disconnect_callback_set(mqtt_, &MqttNorthboundAdapter::disconnectedCallback);
        mosquitto_message_callback_set(mqtt_, &MqttNorthboundAdapter::messageCallback);
        mosquitto_publish_callback_set(mqtt_, &MqttNorthboundAdapter::publishedCallback);
        mosquitto_reconnect_delay_set(mqtt_, 1, 30, true);
        check(mosquitto_connect_async(mqtt_, config_.host.c_str(), config_.port, config_.keepalive), "MQTT connect");
        {
            std::lock_guard<std::mutex> lock(outboundMutex_);
            senderRunning_ = true;
        }
        sender_ = std::thread(&MqttNorthboundAdapter::senderLoop, this);
        check(mosquitto_loop_start(mqtt_), "MQTT loop");
        loopStarted_ = true;
        return true;
    } catch (...) {
        stop();
        throw;
    }
}
void MqttNorthboundAdapter::stop() noexcept {
    if (mqtt_) {
        if (connected_) {
            std::unique_lock<std::mutex> lock(outboundMutex_);
            outboundReady_.wait_for(lock, std::chrono::seconds(2), [this] {
                return outbound_.empty() && !outboundInFlight_;
            });
        }
        if (connected_) {
            edge::UnifiedMessageV2 status;
            status.northboundType = edge::NorthboundType::status;
            status.sourceDescriptor = "gateway";
            status.status = "offline";
            publishRaw(topicFor(status), serialize(status), true, false);
        }
        {
            std::lock_guard<std::mutex> lock(outboundMutex_);
            senderRunning_ = false;
            dropped_ += outbound_.size();
            outbound_.clear();
        }
        outboundReady_.notify_all();
        if (sender_.joinable()) sender_.join();
        mosquitto_disconnect(mqtt_);
        if (loopStarted_) mosquitto_loop_stop(mqtt_, true);
        loopStarted_ = false;
        {
            std::lock_guard<std::mutex> lock(clientMutex_);
            mosquitto_destroy(mqtt_);
            mqtt_ = nullptr;
            pendingMids_.clear();
        }
    }
    connected_ = false;
    if (libraryInitialized_) {
        std::lock_guard<std::mutex> lock(libraryMutex);
        if (--libraryUsers == 0) mosquitto_lib_cleanup();
        libraryInitialized_ = false;
    }
}
std::string MqttNorthboundAdapter::gatewayStatusTopic() const {
    return config_.cloudMode ? "resume/gateway/status/" + config_.clientId
                             : "gateway/" + config_.clientId + "/status";
}
std::string MqttNorthboundAdapter::diagnosticsTopic() const {
    return config_.cloudMode ? gatewayStatusTopic() + "/diagnostics"
                             : "gateway/" + config_.clientId + "/diagnostics";
}
std::string MqttNorthboundAdapter::topicFor(const edge::UnifiedMessageV2& message) const {
    if (message.deviceId.empty() &&
        (message.northboundType == edge::NorthboundType::telemetry ||
         message.northboundType == edge::NorthboundType::attribute ||
         message.northboundType == edge::NorthboundType::command ||
         message.northboundType == edge::NorthboundType::command_result ||
         (message.northboundType == edge::NorthboundType::status && message.sourceDescriptor != "gateway")))
        return {};
    if (message.northboundType == edge::NorthboundType::attribute && message.pointId.empty()) return {};
    const std::string device = config_.cloudMode ? "resume/gateway/devices/" + message.deviceId
                                                  : "device/" + message.deviceId;
    switch (message.northboundType) {
    case edge::NorthboundType::telemetry: return device + "/telemetry";
    case edge::NorthboundType::attribute: return device + "/attribute/" + message.pointId;
    case edge::NorthboundType::status:
        if (message.sourceDescriptor == "gateway")
            return message.operation == "heartbeat"
                ? (config_.cloudMode ? gatewayStatusTopic() + "/heartbeat"
                                     : "gateway/" + config_.clientId + "/heartbeat")
                : gatewayStatusTopic();
        return device + "/status";
    case edge::NorthboundType::alarm:
        return config_.cloudMode ? gatewayStatusTopic() + "/alarm"
                                 : "gateway/" + config_.clientId + "/alarm";
    case edge::NorthboundType::diagnostic: return diagnosticsTopic();
    case edge::NorthboundType::command: return device + "/command";
    case edge::NorthboundType::command_result:
        return config_.cloudMode ? device + "/command/result" : device + "/status";
    }
    return {};
}
std::string MqttNorthboundAdapter::serialize(const edge::UnifiedMessageV2& message) const {
    switch (message.northboundType) {
    case edge::NorthboundType::telemetry: return telemetryJson(message);
    case edge::NorthboundType::status:
        if (message.operation == "heartbeat")
            return "{\"status\":\"online\",\"queue_depth\":" + message.rawValue +
                   ",\"queue_peak\":" + message.cookedValue + "}";
        return "{\"status\":\"" + escapeJson(message.status) + "\",\"detail\":\"" +
               escapeJson(message.detail) + "\"}";
    case edge::NorthboundType::command_result:
        return "{\"status\":\"" + escapeJson(message.status) + "\",\"detail\":\"" +
               escapeJson(message.detail) + "\",\"command_id\":\"" +
               escapeJson(message.correlationId) + "\",\"state\":\"" +
               escapeJson(message.commandState) + "\"}";
    case edge::NorthboundType::diagnostic:
        // Existing diagnostic snapshot is already structured JSON; preserve its wire contract.
        if (message.operation == "legacy_snapshot") return message.cookedValue;
        [[fallthrough]];
    case edge::NorthboundType::attribute:
    case edge::NorthboundType::command:
        return "{\"device_id\":\"" + escapeJson(message.deviceId) + "\",\"point_id\":\"" +
               escapeJson(message.pointId) + "\",\"value\":\"" + escapeJson(message.cookedValue) +
               "\",\"detail\":\"" + escapeJson(message.detail) +
               "\",\"driver_id\":\"" + escapeJson(message.driverId) +
               "\",\"quality\":\"" + iot::toString(message.quality) +
               "\",\"operation\":\"" + escapeJson(message.operation) +
               "\",\"command_id\":\"" + escapeJson(message.correlationId) + "\"}";
    case edge::NorthboundType::alarm:
        return "{\"sequence\":" + std::to_string(message.eventSequence) +
               ",\"key\":\"" + edge::diagnosticJsonEscape(message.pointId) +
               "\",\"action\":\"" + edge::diagnosticJsonEscape(message.operation) +
               "\",\"severity\":\"" + edge::diagnosticJsonEscape(message.status) +
               "\",\"epoch_ms\":" + std::to_string(message.eventEpochMs) + "}";
    }
    return {};
}
PublishResult MqttNorthboundAdapter::publishRaw(const std::string& topic, const std::string& payload,
                                                bool retain, bool telemetry) {
    if (topic.empty() || payload.empty()) { ++dropped_; return {false, "empty topic or payload"}; }
    for (int attempt = 0; attempt < 6; ++attempt) {
        if (connected_) {
            int result = MOSQ_ERR_NO_CONN;
            {
                std::lock_guard<std::mutex> lock(clientMutex_);
                if (mqtt_ && pendingMids_.size() < config_.maxInflight) {
                    const int qos = telemetry ? config_.telemetryQos : 1;
                    const auto send = [&](int* trackerMid) {
                        int observedMid = 0;
                        int* target = trackerMid ? trackerMid : &observedMid;
                        const int code = mosquitto_publish(mqtt_, target, topic.c_str(), static_cast<int>(payload.size()),
                                                 payload.data(), qos, retain);
                        if (code == MOSQ_ERR_SUCCESS) pendingMids_.insert(*target);
                        return code;
                    };
                    result = config_.pipelineMetrics ? publishTracker_.submit(qos, telemetry, send) : send(nullptr);
                }
            }
            if (result == MOSQ_ERR_SUCCESS) { ++published_; return {true, {}}; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(1000, 100 * (1 << attempt))));
    }
    ++failed_;
    return {false, "MQTT disconnected or publish failed"};
}
PublishResult MqttNorthboundAdapter::publish(const edge::UnifiedMessageV2& message) {
    return queueRaw(topicFor(message), serialize(message),
                      message.northboundType == edge::NorthboundType::status &&
                          message.sourceDescriptor == "gateway" && message.operation != "heartbeat",
                      message.northboundType == edge::NorthboundType::telemetry);
}
AdapterMetrics MqttNorthboundAdapter::metrics() const {
    std::size_t pending;
    {
        std::lock_guard<std::mutex> lock(clientMutex_);
        pending = pendingMids_.size();
    }
    std::lock_guard<std::mutex> lock(outboundMutex_);
    return {published_.load(), failed_.load(), dropped_.load(), inbound_.load(),
            connected_.load(), outbound_.size(), outboundPeak_, pending};
}
PublishResult MqttNorthboundAdapter::queueRaw(std::string topic, std::string payload,
                                              bool retain, bool telemetry) {
    if (topic.empty() || payload.empty()) { ++dropped_; return {false, "empty topic or payload"}; }
    {
        std::lock_guard<std::mutex> lock(outboundMutex_);
        if (!senderRunning_ || outbound_.size() >= config_.outboundCapacity) {
            ++dropped_;
            return {false, "MQTT outbound queue stopped or full"};
        }
        outbound_.push_back({std::move(topic), std::move(payload), retain, telemetry});
        outboundPeak_ = std::max(outboundPeak_, outbound_.size());
    }
    outboundReady_.notify_one();
    return {true, {}};
}
void MqttNorthboundAdapter::senderLoop() {
    while (true) {
        Outbound item;
        {
            std::unique_lock<std::mutex> lock(outboundMutex_);
            outboundReady_.wait(lock, [this] { return !senderRunning_ || !outbound_.empty(); });
            if (!senderRunning_) return;
            item = std::move(outbound_.front());
            outbound_.pop_front();
            outboundInFlight_ = true;
        }
        publishRaw(item.topic, item.payload, item.retain, item.telemetry);
        {
            std::lock_guard<std::mutex> lock(outboundMutex_);
            outboundInFlight_ = false;
        }
        outboundReady_.notify_all();
    }
}
void MqttNorthboundAdapter::writePipelineMetrics(std::ostream& out) const { publishTracker_.write(out); }
void MqttNorthboundAdapter::connectedCallback(mosquitto*, void* context, int result) {
    static_cast<MqttNorthboundAdapter*>(context)->onConnected(result);
}
void MqttNorthboundAdapter::disconnectedCallback(mosquitto*, void* context, int result) {
    std::cerr << "MQTT disconnected result=" << result << '\n';
    static_cast<MqttNorthboundAdapter*>(context)->connected_ = false;
}
void MqttNorthboundAdapter::messageCallback(mosquitto*, void* context, const mosquitto_message* message) {
    if (!message || !message->topic || message->payloadlen < 0) return;
    const std::string payload(message->payload ? static_cast<const char*>(message->payload) : "",
                              static_cast<std::size_t>(message->payloadlen));
    static_cast<MqttNorthboundAdapter*>(context)->onMessage(message->topic, payload);
}
void MqttNorthboundAdapter::publishedCallback(mosquitto*, void* context, int mid) {
    static_cast<MqttNorthboundAdapter*>(context)->onPublished(mid);
}
void MqttNorthboundAdapter::onPublished(int mid) {
    std::lock_guard<std::mutex> lock(clientMutex_);
    pendingMids_.erase(mid);
    if (config_.pipelineMetrics) publishTracker_.completed(mid);
}
void MqttNorthboundAdapter::onConnected(int result) {
    std::cerr << "MQTT connected result=" << result << '\n';
    connected_ = result == 0;
    if (!connected_) return;
    const char* topic = config_.cloudMode ? "resume/gateway/devices/+/command" : "device/+/cmd/+";
    mosquitto_subscribe(mqtt_, nullptr, topic, 1);
    if (config_.diagnostics) {
        const auto ack = diagnosticsTopic() + "/ack";
        mosquitto_subscribe(mqtt_, nullptr, ack.c_str(), 1);
    }
    edge::UnifiedMessageV2 status;
    status.northboundType = edge::NorthboundType::status;
    status.sourceDescriptor = "gateway";
    status.status = "online";
    publish(status);
}
void MqttNorthboundAdapter::onMessage(const std::string& topic, const std::string& payload) {
    if (config_.diagnostics && topic == diagnosticsTopic() + "/ack") {
        rapidjson::Document request;
        request.Parse(payload.data(), payload.size());
        std::string key;
        if (!request.HasParseError() && request.IsObject() && request.HasMember("key") &&
            request["key"].IsString() && request["key"].GetStringLength() <= 128)
            key.assign(request["key"].GetString(), request["key"].GetStringLength());
        const bool accepted = !key.empty() && config_.acknowledgeAlarm && config_.acknowledgeAlarm(key);
        queueRaw(diagnosticsTopic() + "/ack/result",
                   "{\"status\":\"" + std::string(accepted ? "acknowledged" : "rejected") +
                   "\",\"key\":\"" + escapeJson(key) + "\"}", false, false);
        return;
    }
    ++inbound_;
    auto parsed = parseCommand(topic, payload);
    if (commandHandler_) commandHandler_(std::move(parsed.message), parsed.error);
}
MqttNorthboundAdapter::ParsedCommand MqttNorthboundAdapter::parseCommand(
    const std::string& topic, const std::string& payload) const {
    const auto route = config_.cloudMode ? router_.routeCloud(topic, payload, config_.resolveCommand)
                                         : router_.route(topic, payload);
    auto command = edge::fromLegacy(route.message);
    command.deviceId = route.deviceId;
    command.driverId = route.driverId;
    command.operation = route.command;
    rapidjson::Document document;
    document.Parse(payload.data(), payload.size());
    std::string error = route.error;
    if (!document.HasParseError() && document.IsObject()) {
        if (document.HasMember("command_id")) {
            if (!document["command_id"].IsString() || document["command_id"].GetStringLength() == 0 ||
                document["command_id"].GetStringLength() > 128)
                error = "command_id must be 1..128 characters";
            else command.correlationId.assign(document["command_id"].GetString(), document["command_id"].GetStringLength());
        }
        if (document.HasMember("timeout_ms")) {
            if (!document["timeout_ms"].IsUint() || !document["timeout_ms"].GetUint())
                error = "timeout_ms must be positive";
            else command.timeout = std::chrono::milliseconds(document["timeout_ms"].GetUint());
        }
        if (document.HasMember("deadline_ms")) {
            const auto maxMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::duration::max()).count();
            if (!document["deadline_ms"].IsUint64() ||
                document["deadline_ms"].GetUint64() > static_cast<std::uint64_t>(maxMs))
                error = "deadline_ms must be a Unix timestamp in milliseconds";
            else {
                const auto now = std::chrono::system_clock::now();
                const auto deadline = std::chrono::system_clock::time_point(
                    std::chrono::milliseconds(document["deadline_ms"].GetUint64()));
                command.deadline = std::chrono::steady_clock::now() + (deadline - now);
            }
        }
    }
    return {std::move(command), std::move(error)};
}

}  // namespace mqmgateway::northbound
