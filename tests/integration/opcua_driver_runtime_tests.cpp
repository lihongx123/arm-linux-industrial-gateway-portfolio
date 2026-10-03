#include "gateway_core.hpp"
#include "opcua_driver.hpp"
#include "acquisition_scheduler.hpp"
#include "command_router.hpp"
#include "probe_timing.hpp"
#include <open62541/server.h>
#include <open62541/server_config_default.h>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

using namespace mqmgateway;
using probe::timing;
namespace {
void check(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
struct OpcUaServer {
    UA_Server* server{nullptr};
    std::atomic<bool> running{false};
    std::thread thread;
    OpcUaServer() {
        server = UA_Server_new(); check(server != nullptr, "UA server allocation");
        check(UA_ServerConfig_setDefault(UA_Server_getConfig(server)) == UA_STATUSCODE_GOOD, "UA server config");
        UA_VariableAttributes attr = UA_VariableAttributes_default;
        UA_Int32 value = 42;
        UA_Variant_setScalar(&attr.value, &value, &UA_TYPES[UA_TYPES_INT32]);
        attr.dataType = UA_TYPES[UA_TYPES_INT32].typeId;
        attr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
        const auto status = UA_Server_addVariableNode(
            server, UA_NODEID_STRING(1, "the.answer"), UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
            UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES), UA_QUALIFIEDNAME(1, "the answer"),
            UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), attr, nullptr, nullptr);
        check(status == UA_STATUSCODE_GOOD, "UA variable");
        UA_Int32 second = 84;
        UA_Variant_setScalar(&attr.value, &second, &UA_TYPES[UA_TYPES_INT32]);
        check(UA_Server_addVariableNode(
            server, UA_NODEID_STRING(1, "second.answer"), UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
            UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES), UA_QUALIFIEDNAME(1, "second answer"),
            UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), attr, nullptr, nullptr) == UA_STATUSCODE_GOOD,
            "UA second variable");
        start();
    }
    void start() {
        check(UA_Server_run_startup(server) == UA_STATUSCODE_GOOD, "UA startup");
        running = true;
        thread = std::thread([this] { while (running) UA_Server_run_iterate(server, true); });
    }
    void stop() {
        running = false; if (thread.joinable()) thread.join();
        UA_Server_run_shutdown(server);
    }
    void set(unsigned value) {
        UA_Int32 v = static_cast<UA_Int32>(value); UA_Variant variant; UA_Variant_init(&variant);
        UA_Variant_setScalar(&variant, &v, &UA_TYPES[UA_TYPES_INT32]);
        check(UA_Server_writeValue(server, UA_NODEID_STRING(1, "the.answer"), variant) == UA_STATUSCODE_GOOD, "UA server write");
    }
    void badStatus() {
        UA_Int32 value = 77; UA_DataValue data; UA_DataValue_init(&data);
        UA_Variant_setScalar(&data.value, &value, &UA_TYPES[UA_TYPES_INT32]);
        data.hasValue = true; data.hasStatus = true; data.status = UA_STATUSCODE_BADOUTOFSERVICE;
        check(UA_Server_writeDataValue(server, UA_NODEID_STRING(1, "the.answer"), data) == UA_STATUSCODE_GOOD,
              "UA server bad status");
    }
    ~OpcUaServer() {
        if (running) stop();
        UA_Server_delete(server);
    }
};
}

int main() {
    try {
        bool invalid = false;
        try { drivers::OpcUaConfig bad; bad.deviceId = "x"; bad.pointId = "p"; bad.endpoint = "opc.tcp://127.0.0.1:4840"; bad.nodeId = "bad"; drivers::OpcUaDriver driver(bad); }
        catch (...) { invalid = true; }
        check(invalid, "bad NodeId must be rejected");
        OpcUaServer server;
        std::mutex mutex; std::condition_variable changed;
        std::vector<edge::UnifiedMessageV2> messages;
        edge::GatewayCore core([&](auto message) {
            std::lock_guard<std::mutex> lock(mutex); messages.push_back(std::move(message)); changed.notify_all();
        });
        drivers::OpcUaConfig config;
        config.deviceId = "opcua-prod"; config.pointId = "answer";
        config.endpoint = "opc.tcp://127.0.0.1:4840"; config.nodeId = "ns=1;s=the.answer";
        config.type = edge::PointValueType::integer; config.writable = true; config.intervalMs = 50; config.responseMs = 500;
        auto driver = std::make_shared<drivers::OpcUaDriver>(config);
        drivers::OpcUaConfig second = config;
        second.deviceId = "opcua-second"; second.pointId = "second"; second.nodeId = "ns=1;s=second.answer";
        auto secondDriver = std::make_shared<drivers::OpcUaDriver>(second);
        drivers::OpcUaConfig missing = config;
        missing.deviceId = "opcua-missing"; missing.pointId = "missing"; missing.nodeId = "ns=1;s=missing";
        auto missingDriver = std::make_shared<drivers::OpcUaDriver>(missing);
        check(core.addDriver(driver) && core.addDevice({config.deviceId, driver->id()}) &&
              core.addPoint({config.deviceId, config.pointId, config.type, "", true, 0}), "OPC UA registration");
        check(core.addDriver(secondDriver) && core.addDevice({second.deviceId, secondDriver->id()}) &&
              core.addPoint({second.deviceId, second.pointId, second.type, "", true, 0}), "OPC UA second device");
        check(core.addDriver(missingDriver) && core.addDevice({missing.deviceId, missingDriver->id()}) &&
              core.addPoint({missing.deviceId, missing.pointId, missing.type, "", false, 0}), "OPC UA missing node");
        check(!core.addDevice({config.deviceId, secondDriver->id()}), "OPC UA duplicate device rejection");
        check(core.start(), "OPC UA core start");
        edge::AcquisitionScheduler scheduler(core.acquisitionDrivers()); scheduler.start();
        auto await = [&](iot::DataType type, const std::string& status, const std::string& value, unsigned count = 1) {
            std::unique_lock<std::mutex> lock(mutex);
            check(changed.wait_for(lock, timing::seconds(8), [&] {
                unsigned seen = 0;
                for (const auto& message : messages)
                    if (message.deviceId == config.deviceId && message.dataType == type && message.status == status &&
                        (value.empty() || message.cookedValue == value) && ++seen >= count) return true;
                return false;
            }), "OPC UA message deadline");
        };
        await(iot::DataType::telemetry, "", "42");
        {
            std::unique_lock<std::mutex> lock(mutex);
            bool first = false, secondSeen = false;
            for (const auto& message : messages) {
                if (message.deviceId == config.deviceId && message.cookedValue == "42" &&
                    message.sourceDescriptor == config.nodeId && message.sourceTimestamp &&
                    message.serverTimestamp && message.protocolStatusCode == UA_STATUSCODE_GOOD) first = true;
                if (message.deviceId == second.deviceId && message.cookedValue == "84" &&
                    message.sourceDescriptor == second.nodeId) secondSeen = true;
            }
            check(first, "OPC UA NodeId, StatusCode and timestamps metadata");
            if (!secondSeen) {
                // The shared scheduler may not have polled the second client yet.
                secondSeen = changed.wait_for(lock, timing::seconds(8), [&] {
                    for (const auto& message : messages)
                        if (message.deviceId == second.deviceId && message.cookedValue == "84") return true;
                    return false;
                });
            }
            check(secondSeen, "OPC UA second device read");
        }
        auto metrics = [&](const std::shared_ptr<drivers::OpcUaDriver>& target) {
            std::ostringstream out; target->appendMetrics(out); return out.str();
        };
        auto until = [&](const auto& done, const char* why) {
            const auto deadline = std::chrono::steady_clock::now() + timing::seconds(12);
            while (!done() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(timing::milliseconds(30));
            check(done(), why);
        };
        until([&] { return metrics(missingDriver).find("\"errors\":0") == std::string::npos; },
              "OPC UA missing NodeId error");
        edge::UnifiedMessageV2 command;
        command.deviceId = config.deviceId; command.pointId = config.pointId; command.dataType = iot::DataType::command;
        command.rawPayload = {0, 0, 0, 77};
        check(core.submit(command), "OPC UA write submit");
        await(iot::DataType::status, "success", "");
        await(iot::DataType::telemetry, "", "77", 2);
        server.badStatus();
        until([&] { return metrics(driver).find("\"last_error_code\":" + std::to_string(UA_STATUSCODE_BADOUTOFSERVICE)) != std::string::npos; },
              "OPC UA bad StatusCode handling");
        server.set(77);
        until([&] { return metrics(driver).find("\"connected\":true") != std::string::npos; },
              "OPC UA recovery after bad StatusCode");
        server.stop();
        until([&] { return metrics(driver).find("\"connected\":false") != std::string::npos; },
              "OPC UA disconnect detection");
        server.start();
        until([&] { return metrics(driver).find("\"connected\":true") != std::string::npos; },
              "OPC UA reconnect");
        iot::CommandRouter router;
        const auto route = router.route("device/opcua-prod/cmd/opcua_write", "{\"value\":88}");
        check(route.accepted && route.message.payload == std::vector<std::uint8_t>({0,0,0,88}), "OPC UA MQTT command");
        scheduler.stop(); core.stop();
        std::cout << "{\"result\":\"PASS\",\"scope\":\"real local open62541 server -> OPC UA client -> GatewayCore\",\"checks\":[\"connect\",\"read\",\"write\",\"bad_nodeid\",\"bad_statuscode\",\"disconnect\",\"reconnect\",\"two_devices\",\"point_mapping\",\"nodeid_metadata\",\"timestamps\",\"command_route\"]}\n";
    } catch (const std::exception& error) {
        std::cerr << "OPC UA runtime FAIL: " << error.what() << '\n'; return 1;
    }
}
