#include "board_driver.hpp"
#include "linux_backends.hpp"
#include "gateway_core.hpp"
#include "acquisition_scheduler.hpp"
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unistd.h>

using namespace mqmgateway;
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void set(const std::string& path, const std::string& value) {
    std::ofstream file(path);
    require(static_cast<bool>(file << value << '\n'), "write simulation attribute");
}
std::string get(const std::string& path) {
    std::ifstream file(path);
    std::string value;
    require(static_cast<bool>(file >> value), "read simulation attribute");
    return value;
}
struct SimulationDirectory {
    std::string path;
    SimulationDirectory() {
        char pattern[] = "/tmp/mqmgateway-adc-pwm-XXXXXX";
        const auto created = mkdtemp(pattern);
        require(created != nullptr, "create simulation directory");
        path = created;
        std::filesystem::create_directory(path + "/pwm0");
    }
    ~SimulationDirectory() { std::filesystem::remove_all(path); }
};
std::vector<std::uint8_t> be64(std::uint64_t value) {
    std::vector<std::uint8_t> bytes(8);
    for (int i = 7; i >= 0; --i) { bytes[i] = static_cast<std::uint8_t>(value); value >>= 8; }
    return bytes;
}
}
int main() {
    try {
        SimulationDirectory sim;
        const auto adcPath = sim.path + "/in_voltage0_raw";
        const auto pwmPath = sim.path + "/pwm0";
        set(adcPath, "2048");
        set(pwmPath + "/period", "500000");
        set(pwmPath + "/duty_cycle", "400000");
        set(pwmPath + "/enable", "1");
        board::AdcConfig adc;
        adc.point.deviceId = "adc01"; adc.point.pointId = "voltage";
        adc.point.path = adcPath; adc.point.intervalMs = 20; adc.point.readLength = 4;
        adc.point.type = edge::PointValueType::integer;
        adc.point.scale = 0.5; adc.point.offset = -10; adc.simulation = true;
        board::PwmConfig pwm;
        pwm.point.deviceId = "pwm01"; pwm.point.pointId = "duty_ns";
        pwm.point.path = pwmPath; pwm.point.intervalMs = 20; pwm.point.readLength = 8;
        pwm.point.type = edge::PointValueType::integer; pwm.point.writable = true;
        pwm.periodNs = 1000000; pwm.simulation = true;
        bool rejected = false;
        try { auto bad = adc; bad.simulation = false; board::AdcBackend invalid(bad); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "production ADC must reject simulation path");
        rejected = false;
        try { auto bad = pwm; bad.simulation = false; board::PwmBackend invalid(bad); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "production PWM must reject simulation path");
        std::mutex mutex;
        std::condition_variable changed;
        std::vector<edge::UnifiedMessageV2> events;
        edge::GatewayCore core([&](auto message) {
            std::lock_guard<std::mutex> lock(mutex);
            events.push_back(std::move(message)); changed.notify_all();
        });
        const auto add = [&](const std::string& kind, const auto& config, auto backend) {
            auto driver = std::make_shared<board::BoardDriver>(kind, config.point, backend);
            require(core.addDriver(driver) && core.addDevice({config.point.deviceId, driver->id()}) &&
                    core.addPoint(*driver->describePoint({})), "register board driver");
        };
        add("adc", adc, std::make_shared<board::AdcBackend>(adc));
        add("pwm", pwm, std::make_shared<board::PwmBackend>(pwm));
        require(core.start(), "start simulated core");
        require(get(pwmPath + "/enable") == "0" && get(pwmPath + "/duty_cycle") == "0" &&
                get(pwmPath + "/period") == "1000000", "PWM starts safely disabled");
        edge::AcquisitionScheduler scheduler(core.acquisitionDrivers());
        scheduler.start();
        const auto await = [&](const std::string& device, iot::DataType type,
                               const std::string& value) {
            std::unique_lock<std::mutex> lock(mutex);
            return changed.wait_for(lock, std::chrono::seconds(3), [&] {
                for (const auto& event : events)
                    if (event.deviceId == device && event.dataType == type &&
                        (type == iot::DataType::status ? event.status : event.cookedValue) == value)
                        return true;
                return false;
            });
        };
        require(await("adc01", iot::DataType::telemetry, "1014"), "ADC scaled value through GatewayCore");
        edge::UnifiedMessageV2 command;
        command.deviceId = "pwm01"; command.pointId = "duty_ns";
        command.dataType = iot::DataType::command; command.rawPayload = be64(250000);
        require(core.submit(command), "PWM command dispatch");
        require(await("pwm01", iot::DataType::status, "ok"), "PWM write status");
        require(get(pwmPath + "/duty_cycle") == "250000" && get(pwmPath + "/enable") == "1",
                "PWM duty and enable applied");
        require(await("pwm01", iot::DataType::telemetry, "250000"), "PWM readback through GatewayCore");
        command.rawPayload = be64(1000001);
        require(core.submit(command), "PWM invalid command routed");
        require(await("pwm01", iot::DataType::status, "error"), "PWM out-of-range rejected");
        require(get(pwmPath + "/duty_cycle") == "250000", "invalid duty leaves output unchanged");
        board::AdcBackend adcProbe(adc);
        adcProbe.open();
        set(adcPath, "-1");
        rejected = false;
        try { (void)adcProbe.read(); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "negative ADC sample rejected");
        scheduler.stop(); core.stop();
        require(get(pwmPath + "/enable") == "0" && get(pwmPath + "/duty_cycle") == "0",
                "PWM stop safe-off");
        std::cout << "{\"result\":\"PASS\",\"adc_scaled\":1014,\"pwm_period_ns\":1000000,"
                     "\"pwm_duty_ns\":250000,\"invalid_duty_rejected\":true,"
                     "\"negative_adc_rejected\":true,\"pwm_safe_off\":true}\n";
    } catch (const std::exception& error) {
        std::cerr << "ADC/PWM simulation FAIL: " << error.what() << '\n';
        return 1;
    }
}
