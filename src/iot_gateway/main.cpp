#include "gateway.hpp"

#include <csignal>
#include <exception>
#include <iostream>
#include <cstdlib>
#include <string>
#include <thread>
#include <stdexcept>
#include <sstream>
#include <charconv>
#include <cmath>

namespace {
volatile std::sig_atomic_t stopRequested = 0;
void signalHandler(int) { stopRequested = 1; }
bool parseBoolean(const std::string& value, const char* name);

unsigned tcpUnsigned(const std::string& field) {
    if (field.empty() || field.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("TCP numeric field invalid");
    const auto value = std::stoull(field);
    if (value > 3600000) throw std::invalid_argument("TCP numeric field too large");
    return static_cast<unsigned>(value);
}
std::vector<std::string> tcpFields(const std::string& input) {
    if (input.empty() || input.back() == ',') throw std::invalid_argument("empty TCP endpoint");
    std::stringstream stream(input);
    std::vector<std::string> fields;
    std::string field;
    while (std::getline(stream, field, ',')) fields.push_back(field);
    return fields;
}
mqmgateway::drivers::ModbusTcpConfig parseModbusTcp(const std::string& input) {
    // id,IPv4,port[,slave,register,poll_ms,response_ms,reconnect_ms]
    const auto fields = tcpFields(input);
    if (fields.size() < 3 || fields.size() > 8)
        throw std::invalid_argument("TCP endpoint: id,IPv4,port[,slave,register,poll_ms,response_ms,reconnect_ms]");
    mqmgateway::drivers::ModbusTcpConfig c;
    c.deviceId = fields[0]; c.address = fields[1];
    unsigned* values[] = {&c.port, &c.slave, &c.registerAddress, &c.pollMs, &c.responseMs, &c.reconnectMs};
    for (std::size_t i = 2; i < fields.size(); ++i) {
        *values[i - 2] = tcpUnsigned(fields[i]);
    }
    return c;
}
mqmgateway::drivers::TcpConfig parseGenericTcp(const std::string& input) {
    // id,IPv4,port[,len16be,reconnect_ms,response_ms]
    const auto fields = tcpFields(input);
    if (fields.size() < 3 || fields.size() > 6)
        throw std::invalid_argument("Generic TCP endpoint: id,IPv4,port[,len16be,reconnect_ms,response_ms]");
    mqmgateway::drivers::TcpConfig c;
    c.deviceId = fields[0]; c.address = fields[1]; c.port = tcpUnsigned(fields[2]);
    if (fields.size() > 3 && fields[3] != "len16be") throw std::invalid_argument("Generic TCP framing must be len16be");
    if (fields.size() > 4) c.reconnectMs = tcpUnsigned(fields[4]);
    if (fields.size() > 5) c.responseMs = tcpUnsigned(fields[5]);
    return c;
}
mqmgateway::drivers::McConfig parseMc(const std::string& input) {
    // id,point,IPv4,port,D-address[,poll_ms,response_ms,reconnect_ms]
    const auto fields = tcpFields(input);
    if (fields.size() < 5 || fields.size() > 8)
        throw std::invalid_argument("MC: id,point,IPv4,port,D-address[,poll_ms,response_ms,reconnect_ms]");
    mqmgateway::drivers::McConfig c;
    c.deviceId = fields[0]; c.pointId = fields[1]; c.address = fields[2];
    c.port = tcpUnsigned(fields[3]);
    if (fields[4].empty() || fields[4].find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("MC D address must be decimal");
    const auto address = std::stoull(fields[4]);
    if (address > 0xFFFFFF) throw std::invalid_argument("MC D address exceeds 24 bits");
    c.registerAddress = static_cast<unsigned>(address);
    unsigned* timing[] = {&c.pollMs, &c.responseMs, &c.reconnectMs};
    for (std::size_t i = 5; i < fields.size(); ++i) *timing[i - 5] = tcpUnsigned(fields[i]);
    return c;
}
mqmgateway::drivers::OpcUaConfig parseOpcUa(const std::string& input) {
    // id,point,endpoint,node_id,type,poll_ms,response_ms,writable
    const auto fields = tcpFields(input);
    if (fields.size() != 8) throw std::invalid_argument("OPC UA: id,point,endpoint,node_id,type,poll_ms,response_ms,writable");
    mqmgateway::drivers::OpcUaConfig c;
    c.deviceId = fields[0]; c.pointId = fields[1]; c.endpoint = fields[2]; c.nodeId = fields[3];
    if (fields[4] == "integer") c.type = mqmgateway::edge::PointValueType::integer;
    else if (fields[4] == "boolean") c.type = mqmgateway::edge::PointValueType::boolean;
    else if (fields[4] == "text") c.type = mqmgateway::edge::PointValueType::text;
    else throw std::invalid_argument("OPC UA type must be integer, boolean or text");
    c.intervalMs = tcpUnsigned(fields[5]); c.responseMs = tcpUnsigned(fields[6]);
    c.writable = parseBoolean(fields[7], "OPC UA writable");
    return c;
}
mqmgateway::drivers::S7Config parseS7(const std::string& input) {
    // id,point,address,rack,slot,db,byte_offset,poll_ms,writable[,port]
    const auto fields = tcpFields(input);
    if (fields.size() < 9 || fields.size() > 10)
        throw std::invalid_argument("S7: id,point,address,rack,slot,db,byte_offset,poll_ms,writable[,port]");
    mqmgateway::drivers::S7Config c;
    c.deviceId = fields[0]; c.pointId = fields[1]; c.address = fields[2];
    c.rack = tcpUnsigned(fields[3]); c.slot = tcpUnsigned(fields[4]);
    c.dbNumber = tcpUnsigned(fields[5]); c.byteOffset = tcpUnsigned(fields[6]);
    c.intervalMs = tcpUnsigned(fields[7]); c.writable = parseBoolean(fields[8], "S7 writable");
    if (fields.size() == 10) c.port = tcpUnsigned(fields[9]);
    return c;
}
std::vector<std::uint8_t> hexBytes(const std::string& value) {
    if (value.size() % 2 || value.size() > 64) throw std::invalid_argument("invalid board hex bytes");
    std::vector<std::uint8_t> bytes;
    for (std::size_t i = 0; i < value.size(); i += 2) {
        unsigned number = 0;
        const auto parsed = std::from_chars(value.data() + i, value.data() + i + 2, number, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + i + 2)
            throw std::invalid_argument("invalid board hex bytes");
        bytes.push_back(static_cast<std::uint8_t>(number));
    }
    return bytes;
}
double boardReal(const std::string& value) {
    std::size_t consumed = 0;
    const double parsed = std::stod(value, &consumed);
    if (consumed != value.size() || !std::isfinite(parsed))
        throw std::invalid_argument("invalid board scale/offset");
    return parsed;
}
mqmgateway::board::SpiConfig parseSpi(const std::string& text) {
    const auto f = tcpFields(text);
    if (f.size() != 12) throw std::invalid_argument("SPI: id,point,path,mode,speed,bits,poll_ms,read_len,cmd_hex,scale,offset,writable");
    mqmgateway::board::SpiConfig c;
    c.point.deviceId=f[0]; c.point.pointId=f[1]; c.point.path=f[2];
    c.mode=tcpUnsigned(f[3]); c.speedHz=tcpUnsigned(f[4]); c.bits=tcpUnsigned(f[5]);
    c.point.intervalMs=tcpUnsigned(f[6]); c.point.readLength=tcpUnsigned(f[7]);
    c.command=hexBytes(f[8]); c.point.scale=boardReal(f[9]); c.point.offset=boardReal(f[10]);
    c.point.writable=parseBoolean(f[11], "SPI writable");
    c.point.type=mqmgateway::edge::PointValueType::integer; return c;
}
mqmgateway::board::I2cConfig parseI2c(const std::string& text) {
    const auto f = tcpFields(text);
    if (f.size() != 10) throw std::invalid_argument("I2C: id,point,path,address,reg_hex,read_len,poll_ms,scale,offset,writable");
    mqmgateway::board::I2cConfig c;
    c.point.deviceId=f[0]; c.point.pointId=f[1]; c.point.path=f[2];
    c.address=tcpUnsigned(f[3]); c.registerBytes=hexBytes(f[4]);
    c.point.readLength=tcpUnsigned(f[5]); c.point.intervalMs=tcpUnsigned(f[6]);
    c.point.scale=boardReal(f[7]); c.point.offset=boardReal(f[8]);
    c.point.writable=parseBoolean(f[9], "I2C writable");
    c.point.type=mqmgateway::edge::PointValueType::integer; return c;
}
mqmgateway::board::GpioConfig parseGpio(const std::string& text) {
    const auto f = tcpFields(text);
    if (f.size() != 7) throw std::invalid_argument("GPIO: id,point,path,line,active_low,output,poll_ms");
    mqmgateway::board::GpioConfig c;
    c.point.deviceId=f[0]; c.point.pointId=f[1]; c.point.path=f[2];
    c.line=tcpUnsigned(f[3]); c.activeLow=parseBoolean(f[4], "GPIO active_low");
    c.output=parseBoolean(f[5], "GPIO output"); c.point.writable=c.output;
    c.point.intervalMs=tcpUnsigned(f[6]); c.point.type=mqmgateway::edge::PointValueType::boolean;
    return c;
}
mqmgateway::board::AdcConfig parseAdc(const std::string& text) {
    const auto f = tcpFields(text);
    if (f.size() != 6) throw std::invalid_argument("ADC: id,point,iio_raw_path,poll_ms,scale,offset");
    mqmgateway::board::AdcConfig c;
    c.point.deviceId=f[0]; c.point.pointId=f[1]; c.point.path=f[2];
    c.point.intervalMs=tcpUnsigned(f[3]); c.point.scale=boardReal(f[4]); c.point.offset=boardReal(f[5]);
    c.point.readLength=4; c.point.type=mqmgateway::edge::PointValueType::integer;
    return c;
}
mqmgateway::board::PwmConfig parsePwm(const std::string& text) {
    const auto f = tcpFields(text);
    if (f.size() != 5) throw std::invalid_argument("PWM: id,point,pwm_channel_path,period_ns,poll_ms");
    mqmgateway::board::PwmConfig c;
    c.point.deviceId=f[0]; c.point.pointId=f[1]; c.point.path=f[2];
    const auto parsed = std::from_chars(f[3].data(), f[3].data() + f[3].size(), c.periodNs);
    if (parsed.ec != std::errc{} || parsed.ptr != f[3].data() + f[3].size())
        throw std::invalid_argument("PWM period_ns must be decimal");
    c.point.intervalMs=tcpUnsigned(f[4]); c.point.readLength=8;
    c.point.type=mqmgateway::edge::PointValueType::integer; c.point.writable=true;
    return c;
}
mqmgateway::serial::Rs485Config parseRs485(const std::string& text) {
    const auto f = tcpFields(text);
    if (f.size() != 5) throw std::invalid_argument("RS485: enabled,rts_on_send,rts_after_send,before_ms,after_ms");
    return {parseBoolean(f[0], "RS485 enabled"), parseBoolean(f[1], "RS485 RTS on send"),
            parseBoolean(f[2], "RS485 RTS after send"), tcpUnsigned(f[3]), tcpUnsigned(f[4])};
}
mqmgateway::drivers::RawUartConfig parseUart(const std::string& text) {
    const auto f = tcpFields(text);
    if (f.size() < 7 || f.size() > 10)
        throw std::invalid_argument("UART: id,point,path,baud,delimiter_byte|fixed:N,reconnect_ms,writable[,bits,parity,stops]");
    mqmgateway::drivers::RawUartConfig c;
    c.deviceId=f[0]; c.pointId=f[1]; c.serial.path=f[2]; c.serial.baud=tcpUnsigned(f[3]);
    if (f[4].rfind("fixed:",0) == 0) { c.delimiterMode=false; c.frameLength=tcpUnsigned(f[4].substr(6)); }
    else { const auto value=tcpUnsigned(f[4]); if (value > 255) throw std::invalid_argument("UART delimiter 0..255"); c.delimiter=value; }
    c.reconnectMs=tcpUnsigned(f[5]); c.writable=parseBoolean(f[6], "UART writable");
    if (f.size() > 7) c.serial.dataBits=tcpUnsigned(f[7]);
    if (f.size() > 8) { if (f[8].size()!=1) throw std::invalid_argument("UART parity N/E/O"); c.serial.parity=f[8][0]; }
    if (f.size() > 9) c.serial.stopBits=tcpUnsigned(f[9]);
    return c;
}
mqmgateway::iot::GatewayConfig::PointPolicyEntry parsePointPolicy(const std::string& text) {
    const auto fields = tcpFields(text);
    if (fields.size() != 5 || fields[0].empty() || fields[1].empty())
        throw std::invalid_argument("point policy: device_id,point_id,cov,deadband,max_report_ms");
    mqmgateway::iot::GatewayConfig::PointPolicyEntry entry;
    entry.deviceId = fields[0];
    entry.pointId = fields[1];
    entry.policy.covEnabled = parseBoolean(fields[2], "point policy cov");
    entry.policy.absoluteDeadband = boardReal(fields[3]);
    if (entry.policy.absoluteDeadband < 0) throw std::invalid_argument("point policy deadband must be nonnegative");
    entry.policy.maxReportInterval = std::chrono::milliseconds(tcpUnsigned(fields[4]));
    return entry;
}
std::size_t positiveCapacity(const std::string& text, const char* name) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument(std::string(name) + " must be a positive decimal integer");
    const auto value = std::stoull(text);
    if (value == 0 || value > 65536)
        throw std::invalid_argument(std::string(name) + " must be in range 1..65536");
    return static_cast<std::size_t>(value);
}

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
        if (key == "--modbus-tcp" || key == "--generic-tcp" || key == "--mc" || key == "--opcua" || key == "--s7") {
            try {
                if (key == "--modbus-tcp") config.modbusTcp.push_back(parseModbusTcp(value));
                else if (key == "--mc") config.mc.push_back(parseMc(value));
                else if (key == "--opcua") config.opcua.push_back(parseOpcUa(value));
                else if (key == "--s7") config.s7.push_back(parseS7(value));
                else config.genericTcp.push_back(parseGenericTcp(value));
            } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 2; }
        }
        else if (key == "--spi" || key == "--i2c" || key == "--gpio" ||
                 key == "--adc" || key == "--pwm") {
            try {
                if (key == "--spi") config.spi.push_back(parseSpi(value));
                else if (key == "--i2c") config.i2c.push_back(parseI2c(value));
                else if (key == "--gpio") config.gpio.push_back(parseGpio(value));
                else if (key == "--adc") config.adc.push_back(parseAdc(value));
                else config.pwm.push_back(parsePwm(value));
            } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 2; }
        }
        else if (key == "--uart" || key == "--uart-rs485" || key == "--rtu-rs485") {
            try {
                if (key == "--uart") config.uart.push_back(parseUart(value));
                else if (key == "--rtu-rs485") config.rtu.rs485=parseRs485(value);
                else {
                    if (config.uart.empty()) throw std::invalid_argument("--uart-rs485 must follow --uart");
                    config.uart.back().serial.rs485=parseRs485(value);
                }
            } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 2; }
        }
        else if (key == "--mqtt-host") config.mqttHost = value;
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
        else if (key == "--telemetry-queue-capacity" || key == "--command-queue-capacity" ||
                 key == "--mqtt-outbound-capacity" ||
                 key == "--command-workers") {
            try {
                const auto number = positiveCapacity(value, key.c_str());
                if (key == "--telemetry-queue-capacity") config.queueCapacity = number;
                else if (key == "--command-queue-capacity") config.commandQueueCapacity = number;
                else if (key == "--mqtt-outbound-capacity") config.mqttOutboundCapacity = number;
                else if (number <= 64) config.commandWorkers = number;
                else throw std::invalid_argument("--command-workers must be in range 1..64");
            } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
        }
        else if (key == "--workers") config.workers = std::stoul(value);
        else if (key == "--point-policy") {
            try { config.pointPolicies.push_back(parsePointPolicy(value)); }
            catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
        }
        else if (key == "--heartbeat-ms") config.heartbeatInterval = std::chrono::milliseconds(std::stoul(value));
        else if (key == "--health-stale-ms" || key == "--health-failures" ||
                 key == "--health-recoveries" || key == "--watchdog-queue-ms") {
            try {
                const auto number = tcpUnsigned(value);
                if (key == "--health-stale-ms") config.health.staleAfter = std::chrono::milliseconds(number);
                else if (key == "--health-failures") config.health.failureThreshold = number;
                else if (key == "--health-recoveries") config.health.recoveryThreshold = number;
                else config.queueWatchdogThreshold = std::chrono::milliseconds(number);
            } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
        }
        else if (key == "--processing-delay-ms") config.processingDelay = std::chrono::milliseconds(std::stoul(value));
        else if (key == "--metrics-file") config.metricsFile = value;
        else if (key == "--pipeline-metrics") {
            if (value != "0" && value != "1") {
                std::cerr << "pipeline-metrics must be 0 or 1\n";
                return 2;
            }
            config.pipelineMetrics = value == "1";
        }
        else if (key == "--diagnostics-mqtt") {
            try { config.diagnosticsMqtt = parseBoolean(value, "--diagnostics-mqtt"); }
            catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
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
