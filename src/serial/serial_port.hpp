#pragma once
#include <linux/serial.h>
#include <string>
namespace mqmgateway::serial {
struct Rs485Config {
    bool enabled{false}, rtsOnSend{true}, rtsAfterSend{false};
    unsigned beforeMs{0}, afterMs{0};
};
struct SerialConfig {
    std::string path;
    unsigned baud{115200}, dataBits{8}, stopBits{1};
    char parity{'N'};
    Rs485Config rs485;
};
class SerialPort {
public:
    explicit SerialPort(SerialConfig config) : mConfig(std::move(config)) {}
    ~SerialPort() { close(); }
    void open();
    void close() noexcept;
    int fd() const noexcept { return mFd; }
    static serial_rs485 rs485State(const Rs485Config& config);
private:
    SerialConfig mConfig;
    int mFd{-1};
};
}
