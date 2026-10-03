#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <sys/types.h>
namespace mqmgateway::transport {
// Reactor-thread owned, numeric IPv4 endpoints: no blocking DNS on the event loop.
class TcpTransport {
public:
    ~TcpTransport() { close(); }
    TcpTransport() = default;
    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;
    bool connect(const std::string& address, unsigned port);
    bool finishConnect();
    void close() noexcept;
    int fd() const { return mFd; }
    ssize_t read(void* data, std::size_t size);
    ssize_t write(const void* data, std::size_t size);
private:
    int mFd{-1};
};
}
