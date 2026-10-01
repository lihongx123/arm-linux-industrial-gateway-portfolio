#include "gateway.hpp"

#include <csignal>
#include <exception>
#include <iostream>
#include <cstdlib>
#include <string>
#include <thread>
#include <stdexcept>

namespace {
volatile std::sig_atomic_t stopRequested = 0;
void signalHandler(int) { stopRequested = 1; }

const char* nonEmptyEnvironmentValue(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? value : nullptr;
}

bool parseBoolean(const std::string& value, const char* name) {
    if (value == "1" || value == "true") return true;
    if (value == "0" || value == "false") return false;
    throw std::invalid_argument(std::string(name) + " must be 0/1 or true/false");
}

void applyCloudEnvironment(mqmgateway::iot::GatewayConfig& config) {
    if (const auto* value = nonEmptyEnvironmentValue("EMQX_HOST")) config.mqttHost = value;
    if (const auto* value = nonEmptyEnvironmentValue("EMQX_PORT")) {
        std::size_t consumed = 0;
        const auto port = std::stoul(value, &consumed);
        if (value[consumed] != '\0' || port == 0 || port > 65535) {
            throw std::invalid_argument("EMQX_PORT must be in range 1..65535");
        }
        config.mqttPort = static_cast<int>(port);
    }
    if (const auto* value = nonEmptyEnvironmentValue("EMQX_CA")) config.mqttCaFile = value;
    if (const auto* value = nonEmptyEnvironmentValue("GATEWAY_MQTT_USERNAME")) config.mqttUsername = value;
    if (const auto* value = nonEmptyEnvironmentValue("GATEWAY_MQTT_PASSWORD")) config.mqttPassword = value;
    if (const auto* value = nonEmptyEnvironmentValue("GATEWAY_MQTT_TLS")) {
        config.mqttTls = parseBoolean(value, "GATEWAY_MQTT_TLS");
    }
}
}

int main(int argc, char** argv) {
    mqmgateway::iot::GatewayConfig config;
    for (int index = 1; index < argc; ++index) {
        const std::string argument(argv[index]);
        const auto separator = argument.find('=');
        const auto key = argument.substr(0, separator);
        const auto value = separator == std::string::npos ? "" : argument.substr(separator + 1);
        if (key == "--mqtt-host") config.mqttHost = value;
        else if (key == "--mqtt-port") config.mqttPort = std::stoi(value);
        else if (key == "--mqtt-tls") {
            try {
                config.mqttTls = parseBoolean(value, "--mqtt-tls");
            } catch (const std::exception& error) {
                std::cerr << error.what() << '\n';
                return 2;
            }
        }
        else if (key == "--cloud") {
            if (!value.empty()) {
                std::cerr << "--cloud does not take a value\n";
                return 2;
            }
            config.cloudMode = true;
        }
        else if (key == "--client-id") config.clientId = value;
        else if (key == "--can-interface") config.canInterface = value;
        else if (key == "--rtu-device") config.rtu.device = value;
        else if (key == "--rtu-baud") config.rtu.baud = std::stoul(value);
        else if (key == "--rtu-slave") config.rtu.slave = std::stoul(value);
        else if (key == "--rtu-register") config.rtu.registerAddress = std::stoul(value);
        else if (key == "--rtu-poll-ms") config.rtu.pollMs = std::stoul(value);
        else if (key == "--rtu-response-ms") config.rtu.responseMs = std::stoul(value);
        else if (key == "--queue-capacity") config.queueCapacity = std::stoul(value);
        else if (key == "--workers") config.workers = std::stoul(value);
        else if (key == "--heartbeat-ms") config.heartbeatInterval = std::chrono::milliseconds(std::stoul(value));
        else if (key == "--processing-delay-ms") config.processingDelay = std::chrono::milliseconds(std::stoul(value));
        else if (key == "--metrics-file") config.metricsFile = value;
        else if (key == "--pipeline-metrics") {
            if (value != "0" && value != "1") {
                std::cerr << "pipeline-metrics must be 0 or 1\n";
                return 2;
            }
            config.pipelineMetrics = value == "1";
        }
        else if (key == "--telemetry-qos") {
            if (value != "0" && value != "1") {
                std::cerr << "telemetry-qos must be 0 or 1\n";
                return 2;
            }
            config.telemetryQos = std::stoi(value);
        }
        else if (key == "--mqtt-inflight") {
            if (value.empty() || value.size() > 5 || value.find_first_not_of("0123456789") != std::string::npos ||
                std::stoul(value) == 0 || std::stoul(value) > 65535) {
                std::cerr << "mqtt-inflight must be 1..65535\n";
                return 2;
            }
            config.mqttMaxInflight = static_cast<unsigned int>(std::stoul(value));
        }
        else {
            std::cerr << "unknown argument: " << argument << '\n';
            return 2;
        }
    }
    try {
        // Secrets stay in the process environment; no credential CLI flags or logging.
        applyCloudEnvironment(config);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    std::signal(SIGPIPE, SIG_IGN);
    try {
        mqmgateway::iot::Gateway gateway(config);
        gateway.start();
        while (!stopRequested && gateway.isRunning()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        const bool reactorFailed = !gateway.isRunning();
        gateway.stop();
        if (reactorFailed) return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
