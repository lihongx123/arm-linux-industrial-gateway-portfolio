#include "register_drivers.hpp"
#include "can_driver.hpp"
#include "modbus_tcp_driver.hpp"
#include "generic_tcp_driver.hpp"
#include <stdexcept>

namespace mqmgateway::drivers {
void registerUartDrivers(edge::GatewayCore& core, const std::vector<RawUartConfig>& uart,
                         std::size_t capacity) {
    for (const auto& c : uart) {
        auto driver = std::make_shared<RawUartDriver>(c, capacity);
        if (!core.addDriver(driver) || !core.addDevice({c.deviceId, driver->id()}) ||
            !core.addPoint(*driver->describePoint({})))
            throw std::invalid_argument("duplicate UART device/point");
    }
}
void registerBoardDrivers(edge::GatewayCore& core, const std::vector<board::SpiConfig>& spi,
                          const std::vector<board::I2cConfig>& i2c,
                          const std::vector<board::GpioConfig>& gpio,
                          const std::vector<board::AdcConfig>& adc,
                          const std::vector<board::PwmConfig>& pwm) {
    const auto add = [&](const std::string& kind, const board::BoardConfig& config,
                         std::shared_ptr<board::IBoardBackend> backend) {
        auto driver = std::make_shared<board::BoardDriver>(kind, config, std::move(backend));
        if (!core.addDriver(driver) || !core.addDevice({config.deviceId, driver->id()}) ||
            !core.addPoint(*driver->describePoint({})))
            throw std::invalid_argument("duplicate board device/point");
    };
    for (const auto& c : spi) add("spi", c.point, std::make_shared<board::SpiBackend>(c));
    for (const auto& c : i2c) add("i2c", c.point, std::make_shared<board::I2cBackend>(c));
    for (const auto& c : gpio) add("gpio", c.point, std::make_shared<board::GpioBackend>(c));
    for (const auto& c : adc) add("adc", c.point, std::make_shared<board::AdcBackend>(c));
    for (const auto& c : pwm) add("pwm", c.point, std::make_shared<board::PwmBackend>(c));
}
void registerTcpDrivers(edge::GatewayCore& core, const std::vector<ModbusTcpConfig>& modbus,
                        const std::vector<TcpConfig>& generic, std::size_t capacity) {
    const auto add = [&](std::shared_ptr<edge::IDeviceDriver> driver, const TcpConfig& config) {
        if (!core.addDriver(driver) || !core.addDevice({config.deviceId, driver->id()}))
            throw std::invalid_argument("duplicate TCP device/driver ID");
    };
    for (const auto& config : modbus) add(std::make_shared<ModbusTcpDriver>(config, capacity), config);
    for (const auto& config : generic) add(std::make_shared<GenericTcpDriver>(config, capacity), config);
}
void registerMcDrivers(edge::GatewayCore& core, const std::vector<McConfig>& configs, std::size_t capacity) {
    for (const auto& config : configs) {
        auto driver = std::make_shared<McDriver>(config, capacity);
        edge::UnifiedMessageV2 description;
        description.deviceId = config.deviceId;
        description.pointId = config.pointId;
        description.legacyAddress = config.registerAddress;
        if (!core.addDriver(driver) || !core.addDevice({config.deviceId, driver->id()}) ||
            !core.addPoint(*driver->describePoint(description)))
            throw std::invalid_argument("duplicate MC device/point");
    }
}
void registerOpcUaDrivers(edge::GatewayCore& core, const std::vector<OpcUaConfig>& configs) {
#ifdef MQM_WITH_OPCUA
    for (const auto& config : configs) {
        auto driver = std::make_shared<OpcUaDriver>(config);
        edge::UnifiedMessageV2 description; description.deviceId = config.deviceId; description.pointId = config.pointId;
        if (!core.addDriver(driver) || !core.addDevice({config.deviceId, driver->id()}) ||
            !core.addPoint(*driver->describePoint(description)))
            throw std::invalid_argument("duplicate OPC UA device/point");
    }
#else
    if (!configs.empty()) throw std::invalid_argument("OPC UA support was not built (enable WITH_OPCUA)");
    (void)core;
#endif
}
void registerS7Drivers(edge::GatewayCore& core, const std::vector<S7Config>& configs) {
#ifdef MQM_WITH_S7
    for (const auto& config : configs) {
        auto driver = std::make_shared<S7Driver>(config);
        edge::UnifiedMessageV2 description; description.deviceId = config.deviceId; description.pointId = config.pointId;
        if (!core.addDriver(driver) || !core.addDevice({config.deviceId, driver->id()}) ||
            !core.addPoint(*driver->describePoint(description)))
            throw std::invalid_argument("duplicate S7 device/point");
    }
#else
    if (!configs.empty()) throw std::invalid_argument("S7 support was not built (enable WITH_S7)");
    (void)core;
#endif
}
void registerSouthboundDrivers(edge::GatewayCore& core, const std::string& canInterface,
                               bool observe, const RtuConfig& rtu, std::size_t capacity) {
    if (!core.addDriver(std::make_shared<CanDriver>(canInterface, observe)) ||
        !core.addDriver(std::make_shared<ModbusRtuDriver>(rtu, capacity)))
        throw std::logic_error("duplicate southbound driver registration");
    if (!rtu.device.empty() &&
        !core.addDevice({"rtu-" + std::to_string(rtu.slave), "modbus_rtu"}))
        throw std::invalid_argument("duplicate RTU device registration");
}
}
