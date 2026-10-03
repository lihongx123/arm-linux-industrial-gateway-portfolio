#include "serial_port.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <stdexcept>
namespace mqmgateway::serial {
static speed_t baudValue(unsigned value) {
    switch (value) {
        case 9600: return B9600; case 19200: return B19200; case 38400: return B38400;
        case 57600: return B57600; case 115200: return B115200;
        default: throw std::invalid_argument("unsupported serial baud");
    }
}
serial_rs485 SerialPort::rs485State(const Rs485Config& c) {
    if (c.beforeMs > 1000 || c.afterMs > 1000) throw std::invalid_argument("RS485 delay exceeds 1000 ms");
    serial_rs485 state{};
    if (c.enabled) {
        state.flags = SER_RS485_ENABLED |
            (c.rtsOnSend ? SER_RS485_RTS_ON_SEND : 0) |
            (c.rtsAfterSend ? SER_RS485_RTS_AFTER_SEND : 0);
        state.delay_rts_before_send = c.beforeMs;
        state.delay_rts_after_send = c.afterMs;
    }
    return state;
}
void SerialPort::open() {
    if (mFd >= 0) return;
    if (mConfig.path.empty() || (mConfig.dataBits != 7 && mConfig.dataBits != 8) ||
        (mConfig.stopBits != 1 && mConfig.stopBits != 2) ||
        (mConfig.parity != 'N' && mConfig.parity != 'E' && mConfig.parity != 'O'))
        throw std::invalid_argument("invalid serial settings");
    const auto speed = baudValue(mConfig.baud);
    const auto rs485 = rs485State(mConfig.rs485);
    mFd = ::open(mConfig.path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (mFd < 0) throw std::runtime_error("serial open failed");
    termios attrs{};
    if (tcgetattr(mFd, &attrs)) { close(); throw std::runtime_error("serial tcgetattr failed"); }
    cfmakeraw(&attrs); cfsetispeed(&attrs, speed); cfsetospeed(&attrs, speed);
    attrs.c_cflag |= CLOCAL | CREAD;
    attrs.c_cflag &= static_cast<tcflag_t>(~(CSIZE | PARENB | PARODD | CSTOPB));
    attrs.c_cflag |= mConfig.dataBits == 8 ? CS8 : CS7;
    if (mConfig.parity != 'N') attrs.c_cflag |= PARENB;
    if (mConfig.parity == 'O') attrs.c_cflag |= PARODD;
    if (mConfig.stopBits == 2) attrs.c_cflag |= CSTOPB;
    attrs.c_cc[VMIN] = 0; attrs.c_cc[VTIME] = 0;
    if (tcsetattr(mFd, TCSANOW, &attrs)) { close(); throw std::runtime_error("serial tcsetattr failed"); }
    if (mConfig.rs485.enabled && ioctl(mFd, TIOCSRS485, &rs485)) {
        close(); throw std::runtime_error("serial RS485 ioctl unavailable");
    }
    tcflush(mFd, TCIOFLUSH);
}
void SerialPort::close() noexcept { if (mFd >= 0) ::close(mFd); mFd = -1; }
}
