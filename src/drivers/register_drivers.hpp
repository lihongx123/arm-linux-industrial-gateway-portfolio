#pragma once
#include "gateway_core.hpp"
#include "modbus_rtu_driver.hpp"
#include "tcp_driver.hpp"
#include "modbus_tcp_driver.hpp"
#include "mc_driver.hpp"
#include "opcua_driver.hpp"
#include "s7_driver.hpp"
#include "raw_uart_driver.hpp"
#include "../board/linux_backends.hpp"

namespace mqmgateway::drivers {
void registerSouthboundDrivers(edge::GatewayCore& core, const std::string& canInterface,
                               bool observe, const RtuConfig& rtu, std::size_t capacity);
void registerTcpDrivers(edge::GatewayCore& core, const std::vector<ModbusTcpConfig>& modbus,
                        const std::vector<TcpConfig>& generic, std::size_t capacity);
void registerMcDrivers(edge::GatewayCore& core, const std::vector<McConfig>& mc, std::size_t capacity);
void registerOpcUaDrivers(edge::GatewayCore& core, const std::vector<OpcUaConfig>& opcua);
void registerS7Drivers(edge::GatewayCore& core, const std::vector<S7Config>& s7);
void registerBoardDrivers(edge::GatewayCore& core, const std::vector<board::SpiConfig>& spi,
                          const std::vector<board::I2cConfig>& i2c,
                          const std::vector<board::GpioConfig>& gpio,
                          const std::vector<board::AdcConfig>& adc,
                          const std::vector<board::PwmConfig>& pwm);
void registerUartDrivers(edge::GatewayCore& core, const std::vector<RawUartConfig>& uart,
                         std::size_t capacity);
}
