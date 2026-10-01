#include <arpa/inet.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <mosquitto.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Config {
    double rate{100.0};
    int duration{30};
    int devices{100};
    int mqttPort{1883};
    int gatewayPid{0};
    std::string interface{"vcan0"};
    std::string output{"/tmp/stress-result.json"};
    std::string metricsFile{"/tmp/stress-metrics.json"};
};

struct ProcSample {
    double cpuSeconds{0.0};
    double rssMb{0.0};
};

std::string argumentValue(const std::string& argument) {
    const auto separator = argument.find('=');
    return separator == std::string::npos ? std::string{} : argument.substr(separator + 1);
}

Config parseArguments(const int argc, char** argv) {
    Config config;
    for (int index = 1; index < argc; ++index) {
        const std::string argument(argv[index]);
        const auto key = argument.substr(0, argument.find('='));
        const auto value = argumentValue(argument);
        if (key == "--rate") config.rate = std::stod(value);
        else if (key == "--duration") config.duration = std::stoi(value);
        else if (key == "--devices") config.devices = std::stoi(value);
        else if (key == "--mqtt-port") config.mqttPort = std::stoi(value);
        else if (key == "--gateway-pid") config.gatewayPid = std::stoi(value);
        else if (key == "--interface") config.interface = value;
        else if (key == "--output") config.output = value;
        else if (key == "--metrics-file") config.metricsFile = value;
        else throw std::runtime_error("unknown argument: " + argument);
    }
    if (config.rate <= 0 || config.duration <= 0 || config.devices <= 0 || config.devices > 2047 || config.gatewayPid <= 0) {
        throw std::runtime_error("invalid rate, duration, devices, or gateway pid");
    }
    return config;
}

ProcSample readProcess(const int pid) {
    ProcSample sample;
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    std::vector<std::string> fields;
    std::string field;
    while (stat >> field) fields.push_back(field);
    if (fields.size() < 24) throw std::runtime_error("gateway process stat unavailable");
    const long ticksPerSecond = sysconf(_SC_CLK_TCK);
    sample.cpuSeconds = (std::stod(fields[13]) + std::stod(fields[14])) / static_cast<double>(ticksPerSecond);

    std::ifstream status("/proc/" + std::to_string(pid) + "/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            std::istringstream input(line.substr(6));
            double kilobytes = 0.0;
            input >> kilobytes;
            sample.rssMb = kilobytes / 1024.0;
            break;
        }
    }
    return sample;
}

std::uint64_t jsonInteger(const std::string& json, const std::string& key) {
    const auto marker = std::string("\"") + key + "\":";
    const auto start = json.find(marker);
    if (start == std::string::npos) return 0;
    const auto valueStart = start + marker.size();
    return std::strtoull(json.c_str() + valueStart, nullptr, 10);
}

std::string readText(const std::string& path) {
    std::ifstream input(path);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

class Subscriber {
public:
    explicit Subscriber(const int port) {
        client_ = mosquitto_new(("arm64-stress-" + std::to_string(getpid())).c_str(), true, this);
        if (!client_) throw std::runtime_error("mosquitto_new failed");
        mosquitto_connect_callback_set(client_, &Subscriber::connectedCallback);
        mosquitto_message_callback_set(client_, &Subscriber::messageCallback);
        if (mosquitto_connect(client_, "127.0.0.1", port, 10) != MOSQ_ERR_SUCCESS) {
            throw std::runtime_error("mosquitto_connect failed");
        }
        if (mosquitto_loop_start(client_) != MOSQ_ERR_SUCCESS) throw std::runtime_error("mosquitto_loop_start failed");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!connected_ && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!connected_) throw std::runtime_error("MQTT subscriber connection timeout");
    }

    ~Subscriber() {
        if (client_) {
            mosquitto_disconnect(client_);
            mosquitto_loop_stop(client_, true);
            mosquitto_destroy(client_);
        }
    }

    std::uint64_t received() const { return received_.load(); }
    std::uint64_t malformed() const { return malformed_.load(); }

    double percentile(const double fraction) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (latencyCount_ == 0) return 0.0;
        const auto wanted = static_cast<std::uint64_t>(std::ceil(static_cast<double>(latencyCount_) * fraction));
        std::uint64_t cumulative = 0;
        for (std::size_t index = 0; index < latencyHistogram_.size(); ++index) {
            cumulative += latencyHistogram_[index];
            if (cumulative >= wanted) return static_cast<double>(index) / 10.0;
        }
        return 60'000.0;
    }

private:
    static void connectedCallback(mosquitto*, void* context, const int result) {
        auto* self = static_cast<Subscriber*>(context);
        if (result == 0) {
            mosquitto_subscribe(self->client_, nullptr, "device/+/telemetry", 1);
            self->connected_ = true;
        }
    }

    static void messageCallback(mosquitto*, void* context, const mosquitto_message* message) {
        auto* self = static_cast<Subscriber*>(context);
        const std::string body(static_cast<const char*>(message->payload), static_cast<std::size_t>(message->payloadlen));
        const std::string marker = "\"payload\":\"";
        const auto start = body.find(marker);
        if (start == std::string::npos || start + marker.size() + 16 > body.size()) {
            ++self->malformed_;
            return;
        }
        std::uint64_t sentNs = 0;
        for (std::size_t index = 0; index < 16; ++index) {
            const char character = body[start + marker.size() + index];
            const int nibble = character >= '0' && character <= '9' ? character - '0'
                : character >= 'a' && character <= 'f' ? character - 'a' + 10
                : character >= 'A' && character <= 'F' ? character - 'A' + 10 : -1;
            if (nibble < 0) {
                ++self->malformed_;
                return;
            }
            sentNs = (sentNs << 4U) | static_cast<std::uint64_t>(nibble);
        }
        const auto nowNs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        if (nowNs < sentNs || nowNs - sentNs > 60'000'000'000ULL) {
            ++self->malformed_;
            return;
        }
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            const double latencyMs = static_cast<double>(nowNs - sentNs) / 1'000'000.0;
            const auto bin = std::min<std::size_t>(
                self->latencyHistogram_.size() - 1,
                static_cast<std::size_t>(std::llround(latencyMs * 10.0)));
            ++self->latencyHistogram_[bin];
            ++self->latencyCount_;
        }
        ++self->received_;
    }

    mosquitto* client_{nullptr};
    std::atomic<bool> connected_{false};
    std::atomic<std::uint64_t> received_{0};
    std::atomic<std::uint64_t> malformed_{0};
    mutable std::mutex mutex_;
    std::vector<std::uint64_t> latencyHistogram_ = std::vector<std::uint64_t>(600'001, 0);
    std::uint64_t latencyCount_{0};
};

int openCan(const std::string& interface) {
    const int descriptor = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (descriptor < 0) throw std::runtime_error("CAN socket failed");
    sockaddr_can address{};
    address.can_family = AF_CAN;
    address.can_ifindex = static_cast<int>(if_nametoindex(interface.c_str()));
    if (address.can_ifindex == 0 || bind(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        close(descriptor);
        throw std::runtime_error("CAN bind failed");
    }
    return descriptor;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto config = parseArguments(argc, argv);
        if (mosquitto_lib_init() != MOSQ_ERR_SUCCESS) throw std::runtime_error("mosquitto_lib_init failed");
        Subscriber subscriber(config.mqttPort);
        const int can = openCan(config.interface);
        const auto procStart = readProcess(config.gatewayPid);
        double peakRssMb = procStart.rssMb;
        const auto wallStart = std::chrono::steady_clock::now();
        const auto endAt = wallStart + std::chrono::seconds(config.duration);
        auto nextSend = wallStart;
        const auto interval = std::chrono::duration<double>(1.0 / config.rate);
        std::uint64_t sent = 0;
        auto nextSample = wallStart + std::chrono::seconds(1);

        while (std::chrono::steady_clock::now() < endAt) {
            const auto now = std::chrono::steady_clock::now();
            if (now < nextSend) {
                std::this_thread::sleep_until(nextSend);
                continue;
            }
            can_frame frame{};
            frame.can_id = static_cast<canid_t>(sent % static_cast<std::uint64_t>(config.devices) + 1);
            frame.can_dlc = 8;
            std::uint64_t timestamp = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            for (int index = 7; index >= 0; --index) {
                frame.data[index] = static_cast<std::uint8_t>(timestamp & 0xffU);
                timestamp >>= 8U;
            }
            if (write(can, &frame, sizeof(frame)) != sizeof(frame)) throw std::runtime_error("CAN write failed");
            ++sent;
            nextSend += std::chrono::duration_cast<std::chrono::steady_clock::duration>(interval);
            if (now >= nextSample) {
                peakRssMb = std::max(peakRssMb, readProcess(config.gatewayPid).rssMb);
                nextSample += std::chrono::seconds(1);
            }
        }
        const auto generationEnd = std::chrono::steady_clock::now();
        const auto receivedAtGenerationEnd = subscriber.received();
        const auto drainDeadline = generationEnd + std::chrono::seconds(5);
        while (subscriber.received() < sent && std::chrono::steady_clock::now() < drainDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const auto measurementEnd = std::chrono::steady_clock::now();
        const auto procEnd = readProcess(config.gatewayPid);
        peakRssMb = std::max(peakRssMb, procEnd.rssMb);
        close(can);

        const auto received = subscriber.received();
        const auto malformed = subscriber.malformed();
        const auto lost = sent > received ? sent - received : 0;
        const double generationSeconds = std::chrono::duration<double>(generationEnd - wallStart).count();
        const double measurementSeconds = std::chrono::duration<double>(measurementEnd - wallStart).count();
        const double receiveRateDuringLoad = static_cast<double>(receivedAtGenerationEnd) / generationSeconds;
        const double finalReceiveRate = static_cast<double>(received) / generationSeconds;
        const double lossRate = sent ? static_cast<double>(lost) / static_cast<double>(sent) : 1.0;
        const double throughputRatio = receiveRateDuringLoad / config.rate;
        const double cpuPercent = 100.0 * (procEnd.cpuSeconds - procStart.cpuSeconds) / measurementSeconds;
        const double p50 = subscriber.percentile(0.50);
        const double p95 = subscriber.percentile(0.95);
        const double p99 = subscriber.percentile(0.99);
        const std::string metrics = readText(config.metricsFile);
        const auto queuePeak = jsonInteger(metrics, "peak_depth");
        const auto rejected = jsonInteger(metrics, "rejected");
        const auto publishFailures = jsonInteger(metrics, "publish_failures");
        const bool processAlive = kill(config.gatewayPid, 0) == 0;

        std::string classification = "DEGRADED";
        int exitCode = 2;
        if (!processAlive || lossRate > 0.05 || throughputRatio < 0.80 || p99 > 1000.0) {
            classification = "FAIL";
            exitCode = 3;
        } else if (lossRate <= 0.001 && throughputRatio >= 0.95 && p99 <= 100.0 && rejected == 0 && malformed == 0) {
            classification = "STABLE";
            exitCode = 0;
        }

        std::ofstream output(config.output);
        output << std::fixed << std::setprecision(6)
               << "{\n  \"classification\":\"" << classification << "\","
               << "\n  \"target_rate\":" << config.rate << ','
               << "\n  \"actual_send_rate\":" << static_cast<double>(sent) / generationSeconds << ','
               << "\n  \"receive_rate_during_load\":" << receiveRateDuringLoad << ','
               << "\n  \"final_receive_rate_after_drain\":" << finalReceiveRate << ','
               << "\n  \"throughput_ratio\":" << throughputRatio << ','
               << "\n  \"duration_seconds\":" << generationSeconds << ','
               << "\n  \"sent\":" << sent << ','
               << "\n  \"received_at_load_end\":" << receivedAtGenerationEnd << ','
               << "\n  \"received\":" << received << ','
               << "\n  \"lost\":" << lost << ','
               << "\n  \"loss_rate\":" << lossRate << ','
               << "\n  \"malformed\":" << malformed << ','
               << "\n  \"p50_ms\":" << p50 << ','
               << "\n  \"p95_ms\":" << p95 << ','
               << "\n  \"p99_ms\":" << p99 << ','
               << "\n  \"cpu_percent\":" << cpuPercent << ','
               << "\n  \"peak_rss_mb\":" << peakRssMb << ','
               << "\n  \"queue_peak\":" << queuePeak << ','
               << "\n  \"rejected\":" << rejected << ','
               << "\n  \"publish_failures\":" << publishFailures << ','
               << "\n  \"process_alive\":" << (processAlive ? "true" : "false") << "\n}\n";
        output.close();
        std::cout << classification << " target=" << config.rate << " receive_during_load=" << receiveRateDuringLoad
                  << " loss=" << lossRate << " p99_ms=" << p99 << '\n';
        return exitCode;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 4;
    }
}
