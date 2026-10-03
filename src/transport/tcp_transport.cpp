#include "tcp_transport.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
namespace mqmgateway::transport {
bool TcpTransport::connect(const std::string& address, unsigned port) {
    close();
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(static_cast<uint16_t>(port));
    if (!port || port > 65535 || inet_pton(AF_INET, address.c_str(), &peer.sin_addr) != 1) return false;
    mFd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (mFd < 0) return false;
    const int enabled = 1;
    setsockopt(mFd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
    if (::connect(mFd, reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) == 0 || errno == EINPROGRESS) return true;
    close();
    return false;
}
bool TcpTransport::finishConnect() {
    int error = 0;
    socklen_t size = sizeof(error);
    return getsockopt(mFd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0;
}
void TcpTransport::close() noexcept { if (mFd >= 0) ::close(mFd); mFd = -1; }
ssize_t TcpTransport::read(void* data, std::size_t size) { return recv(mFd, data, size, 0); }
ssize_t TcpTransport::write(const void* data, std::size_t size) { return send(mFd, data, size, MSG_NOSIGNAL); }
}
