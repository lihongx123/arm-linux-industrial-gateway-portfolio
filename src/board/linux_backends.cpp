#include "linux_backends.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <linux/gpio.h>
#include <stdexcept>
namespace mqmgateway::board {
SpiBackend::SpiBackend(SpiConfig c) : mConfig(std::move(c)) {
    if (mConfig.point.path.rfind("/dev/spidev", 0) || mConfig.mode > 3 ||
        mConfig.speedHz < 1000 || mConfig.speedHz > 100000000 || mConfig.bits != 8 ||
        mConfig.command.size() > 32) throw std::invalid_argument("invalid SPI config");
}
void SpiBackend::open() {
    mFd = ::open(mConfig.point.path.c_str(), O_RDWR | O_CLOEXEC);
    if (mFd < 0) throw std::runtime_error("open spidev failed");
    const auto mode = static_cast<std::uint8_t>(mConfig.mode);
    const auto bits = static_cast<std::uint8_t>(mConfig.bits);
    const auto speed = static_cast<std::uint32_t>(mConfig.speedHz);
    if (ioctl(mFd, SPI_IOC_WR_MODE, &mode) || ioctl(mFd, SPI_IOC_WR_BITS_PER_WORD, &bits) ||
        ioctl(mFd, SPI_IOC_WR_MAX_SPEED_HZ, &speed)) { close(); throw std::runtime_error("SPI setup ioctl failed"); }
}
void SpiBackend::close() noexcept { if (mFd >= 0) ::close(mFd); mFd = -1; }
std::vector<std::uint8_t> SpiBackend::read() {
    if (mFd < 0) throw std::runtime_error("SPI closed");
    const auto size = mConfig.command.size() + mConfig.point.readLength;
    std::vector<std::uint8_t> tx(size), rx(size);
    std::copy(mConfig.command.begin(), mConfig.command.end(), tx.begin());
    spi_ioc_transfer transfer{};
    transfer.tx_buf = reinterpret_cast<__u64>(tx.data()); transfer.rx_buf = reinterpret_cast<__u64>(rx.data());
    transfer.len = size; transfer.speed_hz = mConfig.speedHz; transfer.bits_per_word = mConfig.bits;
    if (ioctl(mFd, SPI_IOC_MESSAGE(1), &transfer) != static_cast<int>(size)) throw std::runtime_error("SPI transfer failed/short");
    return {rx.begin() + mConfig.command.size(), rx.end()};
}
void SpiBackend::write(const std::vector<std::uint8_t>& bytes) {
    if (mFd < 0) throw std::runtime_error("SPI closed");
    std::vector<std::uint8_t> tx = mConfig.command;
    tx.insert(tx.end(), bytes.begin(), bytes.end());
    spi_ioc_transfer transfer{}; transfer.tx_buf = reinterpret_cast<__u64>(tx.data());
    transfer.len = tx.size(); transfer.speed_hz = mConfig.speedHz; transfer.bits_per_word = mConfig.bits;
    if (ioctl(mFd, SPI_IOC_MESSAGE(1), &transfer) != static_cast<int>(tx.size())) throw std::runtime_error("SPI write failed/short");
}
I2cBackend::I2cBackend(I2cConfig c) : mConfig(std::move(c)) {
    if (mConfig.point.path.rfind("/dev/i2c-", 0) || mConfig.address < 0x08 ||
        mConfig.address > 0x77 || mConfig.registerBytes.size() > 16)
        throw std::invalid_argument("invalid I2C config");
}
void I2cBackend::open() { mFd = ::open(mConfig.point.path.c_str(), O_RDWR | O_CLOEXEC); if (mFd < 0) throw std::runtime_error("open i2c failed"); }
void I2cBackend::close() noexcept { if (mFd >= 0) ::close(mFd); mFd = -1; }
std::vector<std::uint8_t> I2cBackend::read() {
    if (mFd < 0) throw std::runtime_error("I2C closed");
    auto reg = mConfig.registerBytes;
    std::vector<std::uint8_t> data(mConfig.point.readLength);
    i2c_msg msg[2]{};
    unsigned count = 0;
    if (!reg.empty()) { msg[count++] = {static_cast<__u16>(mConfig.address), 0, static_cast<__u16>(reg.size()), reg.data()}; }
    msg[count++] = {static_cast<__u16>(mConfig.address), I2C_M_RD, static_cast<__u16>(data.size()), data.data()};
    i2c_rdwr_ioctl_data request{msg, count};
    if (ioctl(mFd, I2C_RDWR, &request) != static_cast<int>(count)) throw std::runtime_error("I2C read failed/short");
    return data;
}
void I2cBackend::write(const std::vector<std::uint8_t>& bytes) {
    if (mFd < 0) throw std::runtime_error("I2C closed");
    std::vector<std::uint8_t> data = mConfig.registerBytes;
    data.insert(data.end(), bytes.begin(), bytes.end());
    i2c_msg msg{static_cast<__u16>(mConfig.address), 0, static_cast<__u16>(data.size()), data.data()};
    i2c_rdwr_ioctl_data request{&msg, 1};
    if (ioctl(mFd, I2C_RDWR, &request) != 1) throw std::runtime_error("I2C write failed");
}
GpioBackend::GpioBackend(GpioConfig c) : mConfig(std::move(c)) {
    if (mConfig.point.path.rfind("/dev/gpiochip", 0) || mConfig.point.readLength != 1 ||
        mConfig.point.writable != mConfig.output)
        throw std::invalid_argument("invalid GPIO config");
}
void GpioBackend::open() {
    mChip = ::open(mConfig.point.path.c_str(), O_RDONLY | O_CLOEXEC);
    if (mChip < 0) throw std::runtime_error("open gpiochip failed");
    gpiochip_info info{};
    if (ioctl(mChip, GPIO_GET_CHIPINFO_IOCTL, &info) || mConfig.line >= info.lines) {
        close(); throw std::runtime_error("GPIO line outside chip range");
    }
    gpio_v2_line_request req{};
    req.offsets[0] = mConfig.line; req.num_lines = 1;
    req.config.flags = (mConfig.output ? GPIO_V2_LINE_FLAG_OUTPUT : GPIO_V2_LINE_FLAG_INPUT) |
                       (mConfig.activeLow ? GPIO_V2_LINE_FLAG_ACTIVE_LOW : 0);
    if (ioctl(mChip, GPIO_V2_GET_LINE_IOCTL, &req)) { close(); throw std::runtime_error("GPIO line request failed"); }
    mLine = req.fd;
}
void GpioBackend::close() noexcept {
    if (mLine >= 0) ::close(mLine);
    if (mChip >= 0) ::close(mChip);
    mLine = mChip = -1;
}
std::vector<std::uint8_t> GpioBackend::read() {
    if (mLine < 0) throw std::runtime_error("GPIO closed");
    gpio_v2_line_values values{}; values.mask = 1;
    if (ioctl(mLine, GPIO_V2_LINE_GET_VALUES_IOCTL, &values)) throw std::runtime_error("GPIO read failed");
    return {static_cast<std::uint8_t>(values.bits & 1)};
}
void GpioBackend::write(const std::vector<std::uint8_t>& bytes) {
    if (!mConfig.output || mLine < 0 || bytes.size() != 1 || bytes[0] > 1) throw std::runtime_error("GPIO write invalid");
    gpio_v2_line_values values{}; values.mask = 1; values.bits = bytes[0];
    if (ioctl(mLine, GPIO_V2_LINE_SET_VALUES_IOCTL, &values)) throw std::runtime_error("GPIO write failed");
}
}
