#include "opcua_driver.hpp"
#include <open62541/client.h>
#include <open62541/client_config_default.h>
#include <open62541/client_highlevel.h>
#include <limits>
#include <stdexcept>

namespace mqmgateway::drivers {
namespace {
UA_NodeId node(const std::string& text) {
    UA_NodeId value{};
    // UA_NodeId_parse reads the input; only the resulting NodeId owns memory.
    // UA_STRING_ALLOC here would leak an input buffer on every poll/write.
    if (UA_NodeId_parse(&value, UA_STRING(const_cast<char*>(text.c_str()))) != UA_STATUSCODE_GOOD) {
        UA_NodeId_clear(&value);
        throw std::invalid_argument("OPC UA NodeId must use ns=...;i=... or ns=...;s=...");
    }
    return value;
}
void markGood(std::atomic<std::uint64_t>& consecutive, std::atomic<std::int64_t>& lastSuccess,
              std::atomic<unsigned>& lastError) {
    consecutive = 0; lastError = 0;
    lastSuccess = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
}

OpcUaDriver::OpcUaDriver(OpcUaConfig config) : config_(std::move(config)) {
    if (config_.deviceId.empty() || config_.pointId.empty() || config_.endpoint.empty() ||
        config_.nodeId.empty() || !config_.intervalMs || !config_.responseMs ||
        (config_.type != edge::PointValueType::integer && config_.type != edge::PointValueType::boolean &&
         config_.type != edge::PointValueType::text))
        throw std::invalid_argument("invalid OPC UA point configuration");
    auto parsed = node(config_.nodeId);
    UA_NodeId_clear(&parsed);
}
std::optional<edge::PointDefinition> OpcUaDriver::describePoint(const edge::UnifiedMessageV2& message) const {
    if (message.deviceId != config_.deviceId && !message.deviceId.empty()) return std::nullopt;
    if (!message.pointId.empty() && message.pointId != config_.pointId) return std::nullopt;
    return edge::PointDefinition{config_.deviceId, config_.pointId, config_.type, "", config_.writable, 0};
}
void OpcUaDriver::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) return;
    client_ = UA_Client_new();
    if (!client_) throw std::runtime_error("UA_Client_new failed");
    UA_ClientConfig_setDefault(UA_Client_getConfig(static_cast<UA_Client*>(client_)));
    UA_Client_getConfig(static_cast<UA_Client*>(client_))->timeout = config_.responseMs;
    started_ = true;
}
void OpcUaDriver::stop() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    started_ = false; connected_ = false;
    if (client_) {
        auto* client = static_cast<UA_Client*>(client_);
        UA_Client_disconnect(client); UA_Client_delete(client); client_ = nullptr;
    }
}
bool OpcUaDriver::connectLocked() {
    if (!started_ || !client_) return false;
    if (connected_) return true;
    const auto status = UA_Client_connect(static_cast<UA_Client*>(client_), config_.endpoint.c_str());
    if (status != UA_STATUSCODE_GOOD) { failLocked(status); return false; }
    connected_ = true; ++reconnects_; return true;
}
void OpcUaDriver::failLocked(unsigned code) {
    ++errors_; ++consecutiveFailures_; lastErrorCode_ = code;
    if (code == UA_STATUSCODE_BADTIMEOUT) ++timeouts_;
    connected_ = false;
    if (client_) UA_Client_disconnect(static_cast<UA_Client*>(client_));
}
bool OpcUaDriver::emitValueLocked(const std::vector<std::uint8_t>& bytes,
                                  std::optional<std::chrono::system_clock::time_point> source,
                                  std::optional<std::chrono::system_clock::time_point> server) {
    edge::UnifiedMessageV2 message;
    message.deviceId = config_.deviceId; message.pointId = config_.pointId; message.driverId = id();
    message.legacyProtocol = iot::Protocol::opcua; message.rawPayload = bytes;
    message.sourceDescriptor = config_.nodeId;
    message.protocolStatusCode = UA_STATUSCODE_GOOD;
    message.sourceTimestamp = source; message.serverTimestamp = server;
    message.sourceTime = source.value_or(std::chrono::system_clock::now());
    message.enqueuedAt = std::chrono::steady_clock::now();
    if (emit_) emit_(std::move(message));
    return true;
}
void OpcUaDriver::acquire(std::chrono::steady_clock::time_point) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connectLocked()) return;
    auto* client = static_cast<UA_Client*>(client_);
    UA_ReadValueId item; UA_ReadValueId_init(&item);
    item.nodeId = node(config_.nodeId); item.attributeId = UA_ATTRIBUTEID_VALUE;
    UA_ReadRequest request; UA_ReadRequest_init(&request);
    request.nodesToRead = &item; request.nodesToReadSize = 1;
    request.timestampsToReturn = UA_TIMESTAMPSTORETURN_BOTH;
    auto response = UA_Client_Service_read(client, request);
    UA_NodeId_clear(&item.nodeId);
    UA_StatusCode status = response.responseHeader.serviceResult;
    if (status == UA_STATUSCODE_GOOD && response.resultsSize != 1) status = UA_STATUSCODE_BADUNEXPECTEDERROR;
    const UA_DataValue* data = status == UA_STATUSCODE_GOOD ? &response.results[0] : nullptr;
    if (data && data->hasStatus) status = data->status;
    if (status != UA_STATUSCODE_GOOD || !data || !data->hasValue || !data->value.data ||
        !UA_Variant_isScalar(&data->value)) {
        UA_ReadResponse_clear(&response);
        failLocked(status == UA_STATUSCODE_GOOD ? UA_STATUSCODE_BADTYPEMISMATCH : status);
        return;
    }
    const UA_Variant& value = data->value;
    std::vector<std::uint8_t> bytes;
    if (value.type == &UA_TYPES[UA_TYPES_BOOLEAN] && config_.type == edge::PointValueType::boolean) {
        bytes = {static_cast<std::uint8_t>(*static_cast<UA_Boolean*>(value.data) ? 1 : 0)};
    } else if (config_.type == edge::PointValueType::integer &&
               (value.type == &UA_TYPES[UA_TYPES_INT32] || value.type == &UA_TYPES[UA_TYPES_UINT32])) {
        const auto number = value.type == &UA_TYPES[UA_TYPES_INT32]
            ? static_cast<std::int64_t>(*static_cast<UA_Int32*>(value.data))
            : static_cast<std::int64_t>(*static_cast<UA_UInt32*>(value.data));
        if (number < 0 || number > std::numeric_limits<std::uint32_t>::max()) {
            UA_ReadResponse_clear(&response); failLocked(UA_STATUSCODE_BADTYPEMISMATCH); return;
        }
        const auto n = static_cast<std::uint32_t>(number);
        bytes = {static_cast<std::uint8_t>(n >> 24), static_cast<std::uint8_t>(n >> 16),
                 static_cast<std::uint8_t>(n >> 8), static_cast<std::uint8_t>(n)};
    } else if (config_.type == edge::PointValueType::text && value.type == &UA_TYPES[UA_TYPES_STRING]) {
        auto& string = *static_cast<UA_String*>(value.data);
        bytes.assign(string.data, string.data + string.length);
    } else { UA_ReadResponse_clear(&response); failLocked(UA_STATUSCODE_BADTYPEMISMATCH); return; }
    auto timestamp = [](UA_DateTime date) {
        return std::chrono::system_clock::time_point(std::chrono::milliseconds(
            (date - UA_DATETIME_UNIX_EPOCH) / UA_DATETIME_MSEC));
    };
    const auto source = data->hasSourceTimestamp
        ? std::optional<std::chrono::system_clock::time_point>(timestamp(data->sourceTimestamp)) : std::nullopt;
    const auto server = data->hasServerTimestamp
        ? std::optional<std::chrono::system_clock::time_point>(timestamp(data->serverTimestamp)) : std::nullopt;
    UA_ReadResponse_clear(&response);
    ++reads_; markGood(consecutiveFailures_, lastSuccessEpochMs_, lastErrorCode_);
    emitValueLocked(bytes, source, server);
}
bool OpcUaDriver::submit(const edge::UnifiedMessageV2& command) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_.writable || !started_ || command.deviceId != config_.deviceId ||
        (!command.pointId.empty() && command.pointId != config_.pointId) || !connectLocked()) return false;
    auto nodeId = node(config_.nodeId); UA_Variant value; UA_Variant_init(&value);
    UA_StatusCode status = UA_STATUSCODE_BADTYPEMISMATCH;
    if (config_.type == edge::PointValueType::boolean && command.rawPayload.size() == 1) {
        UA_Boolean v = command.rawPayload[0] != 0; status = UA_Variant_setScalarCopy(&value, &v, &UA_TYPES[UA_TYPES_BOOLEAN]);
    } else if (config_.type == edge::PointValueType::integer && command.rawPayload.size() == 4) {
        const auto v = static_cast<UA_Int32>((std::uint32_t(command.rawPayload[0]) << 24) |
            (std::uint32_t(command.rawPayload[1]) << 16) | (std::uint32_t(command.rawPayload[2]) << 8) | command.rawPayload[3]);
        status = UA_Variant_setScalarCopy(&value, &v, &UA_TYPES[UA_TYPES_INT32]);
    } else if (config_.type == edge::PointValueType::text) {
        UA_String v = UA_STRING_ALLOC(std::string(command.rawPayload.begin(), command.rawPayload.end()).c_str());
        status = UA_Variant_setScalarCopy(&value, &v, &UA_TYPES[UA_TYPES_STRING]); UA_String_clear(&v);
    }
    if (status == UA_STATUSCODE_GOOD) status = UA_Client_writeValueAttribute(static_cast<UA_Client*>(client_), nodeId, &value);
    UA_Variant_clear(&value); UA_NodeId_clear(&nodeId);
    auto result = command; result.dataType = iot::DataType::status; result.direction = iot::Direction::northbound;
    result.sourceDescriptor = config_.nodeId; result.sourceTime = std::chrono::system_clock::now();
    if (status == UA_STATUSCODE_GOOD) {
        ++writes_; markGood(consecutiveFailures_, lastSuccessEpochMs_, lastErrorCode_);
        result.status = "success"; result.detail = "OPC UA write acknowledged"; result.quality = iot::Quality::good;
    } else { failLocked(status); result.status = status == UA_STATUSCODE_BADTIMEOUT ? "timeout" : "failed";
        result.protocolStatusCode = status;
        result.detail = "OPC UA StatusCode " + std::to_string(status); result.quality = iot::Quality::unavailable; }
    if (emit_) emit_(std::move(result));
    return true;
}
void OpcUaDriver::appendMetrics(std::ostream& out) const {
    out << ",\"" << id() << "\":{\"connected\":" << (connected_ ? "true" : "false")
        << ",\"reads\":" << reads_.load() << ",\"writes\":" << writes_.load()
        << ",\"errors\":" << errors_.load() << ",\"reconnects\":" << reconnects_.load()
        << ",\"timeouts\":" << timeouts_.load() << ",\"consecutive_failures\":" << consecutiveFailures_.load()
        << ",\"last_success_epoch_ms\":" << lastSuccessEpochMs_.load()
        << ",\"last_error_code\":" << lastErrorCode_.load() << '}';
}
}  // namespace mqmgateway::drivers
