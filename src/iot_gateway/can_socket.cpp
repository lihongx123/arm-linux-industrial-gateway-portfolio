#include "can_socket.hpp"

#include <cerrno>
#include <cstring>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace mqmgateway::iot {

CanSocket::CanSocket(std::string interfaceName, bool observe)
    : interfaceName_(std::move(interfaceName)), observe_(observe) {}

CanSocket::~CanSocket() {
    close();
}

void CanSocket::open() {
    close();
    socket_ = ::socket(PF_CAN, SOCK_RAW | SOCK_NONBLOCK, CAN_RAW);
    if (socket_ < 0) {
        throw std::runtime_error("socket(PF_CAN): " + std::string(std::strerror(errno)));
    }
    ifreq request{};
    std::strncpy(request.ifr_name, interfaceName_.c_str(), IFNAMSIZ - 1);
    if (::ioctl(socket_, SIOCGIFINDEX, &request) < 0) {
        const auto error = std::string(std::strerror(errno));
        close();
        throw std::runtime_error("CAN interface " + interfaceName_ + ": " + error);
    }
    sockaddr_can address{};
    address.can_family = AF_CAN;
    address.can_ifindex = request.ifr_ifindex;
    if (::bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        const auto error = std::string(std::strerror(errno));
        close();
        throw std::runtime_error("bind(" + interfaceName_ + "): " + error);
    }
}

void CanSocket::close() {
    if (socket_ >= 0) {
        ::close(socket_);
        socket_ = -1;
    }
}

bool CanSocket::isOpen() const {
    return socket_ >= 0;
}

bool CanSocket::send(const UnifiedMessage& message) {
    if (message.payload.size() > CAN_MAX_DLEN || socket_ < 0) {
        return false;
    }
    can_frame frame{};
    if (message.address > CAN_EFF_MASK) return false;
    frame.can_id = message.address | (message.address > CAN_SFF_MASK ? CAN_EFF_FLAG : 0U);
    frame.can_dlc = static_cast<__u8>(message.payload.size());
    std::copy(message.payload.begin(), message.payload.end(), frame.data);
    return ::write(socket_, &frame, sizeof(frame)) == sizeof(frame);
}

bool CanSocket::receiveReady(UnifiedMessage& message) {
    can_frame frame{};
    if (::read(socket_, &frame, sizeof(frame)) != sizeof(frame)) {
        if (observe_ && errno != EAGAIN && errno != EINTR) ++readErrors_;
        return false;
    }
    const auto receivedAt = observe_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (observe_) ++received_;
    if (frame.can_dlc > CAN_MAX_DLEN || (frame.can_id & (CAN_RTR_FLAG | CAN_ERR_FLAG)) != 0) {
        if (observe_) ++rejected_;
        return false;
    }
    message.timestamp = std::chrono::system_clock::now();
    message.enqueuedAt = std::chrono::steady_clock::now();
    message.deviceId = "can-" + std::to_string(frame.can_id & CAN_EFF_MASK);
    message.protocol = Protocol::can;
    message.direction = Direction::northbound;
    message.dataType = DataType::telemetry;
    message.quality = Quality::good;
    message.address = frame.can_id & CAN_EFF_MASK;
    message.payload.assign(frame.data, frame.data + frame.can_dlc);
    if (observe_) {
        ++parsed_;
        parseLatency_.observe(std::chrono::steady_clock::now() - receivedAt);
    }
    return true;
}

void CanSocket::writeMetrics(std::ostream& out) const {
    out << "{\"frames_read\":" << received_.load() << ",\"parsed\":" << parsed_.load()
        << ",\"rejected_frames\":" << rejected_.load() << ",\"read_errors\":" << readErrors_.load()
        << ",\"parse\":";
    parseLatency_.write(out);
    out << '}';
}

}  // namespace mqmgateway::iot
