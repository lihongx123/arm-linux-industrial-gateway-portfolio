#include "southbound_reactor.hpp"
#include <algorithm>
#include <cerrno>
#include <iostream>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
namespace mqmgateway::iot {
SouthboundReactor::SouthboundReactor(CanSocket& can,RtuConfig config,std::size_t capacity,Emit emit)
    :can_(can),config_(std::move(config)),emit_(std::move(emit)),capacity_(capacity) {
    if(config_.slave<1 || config_.slave>247 || config_.registerAddress>65535 || !config_.pollMs || !config_.responseMs)
        throw std::invalid_argument("invalid RTU configuration");
    try {
        epoll_=epoll_create1(EPOLL_CLOEXEC);wake_=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
        if(epoll_<0 || wake_<0)throw std::runtime_error("southbound epoll/eventfd");
        const auto add=[&](int fd) {
            epoll_event e{};e.events=EPOLLIN;e.data.fd=fd;
            if(epoll_ctl(epoll_,EPOLL_CTL_ADD,fd,&e))throw std::runtime_error("southbound registration");
        };
        add(can_.nativeHandle());add(wake_);
        if(!config_.device.empty()) {
            serial_=std::make_unique<serial::TermiosRtuTransport>(config_.device,config_.baud);
            serial_->open();add(serial_->nativeHandle());
        }
        nextPoll_=Clock::now()+std::chrono::milliseconds(config_.pollMs);
        std::cerr<<"SOUTHBOUND pid="<<getpid()<<" epoll="<<epoll_<<" can_fd="<<can_.nativeHandle()
                 <<" serial_fd="<<(serial_?serial_->nativeHandle():-1)<<"\n";
    } catch(...) {if(epoll_>=0)close(epoll_);if(wake_>=0)close(wake_);throw;}
}
SouthboundReactor::~SouthboundReactor(){if(epoll_>=0)close(epoll_);if(wake_>=0)close(wake_);}
void SouthboundReactor::wake(){uint64_t one=1;const auto n=write(wake_,&one,sizeof(one));(void)n;}
bool SouthboundReactor::submit(UnifiedMessage command) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!serial_ || command.slave!=config_.slave || commands_.size()>=capacity_) {++rtuRejected_;return false;}
    commands_.push_back(std::move(command));wake();return true;
}
void SouthboundReactor::updateSerial(bool writable) {
    epoll_event e{};e.events=EPOLLIN|(writable?EPOLLOUT:0U);e.data.fd=serial_->nativeHandle();
    if(epoll_ctl(epoll_,EPOLL_CTL_MOD,e.data.fd,&e))throw std::runtime_error("serial update");
}
void SouthboundReactor::finish(Quality quality,const serial::ByteBuffer& response) {
    if(!pending_)return;
    auto message=pending_->message;const bool command=pending_->command;
    pending_.reset();updateSerial(false);
    quietUntil_=Clock::now()+std::chrono::milliseconds(2);
    message.direction=Direction::northbound;message.quality=quality;message.timestamp=std::chrono::system_clock::now();
    message.enqueuedAt=Clock::now();
    if(command) {message.dataType=DataType::status;if(quality==Quality::good)++rtuWrites_;}
    else message.dataType=DataType::telemetry;
    if(!response.empty())message.payload=response;
    if(command || quality==Quality::good)emit_(std::move(message));
}
void SouthboundReactor::flush() {
    if(!pending_)return;
    auto& p=*pending_;
    while(p.offset<p.request.size()) {
        auto n=write(serial_->nativeHandle(),p.request.data()+p.offset,p.request.size()-p.offset);
        if(n<0 && errno==EINTR)continue;
        if(n<0 && errno==EAGAIN){updateSerial(true);return;}
        if(n<=0){finish(Quality::unavailable);return;}
        p.offset+=static_cast<std::size_t>(n);
    }
    updateSerial(false);
}
void SouthboundReactor::tick() {
    if(!serial_)return;
    auto now=Clock::now();
    if(pending_ && now>=pending_->deadline) {
        ++rtuTimeouts_;finish(Quality::timeout);
        serial_->discardInput();
        quietUntil_=now+std::chrono::milliseconds(config_.responseMs);
    }
    if(pending_ || now<quietUntil_)return;
    std::optional<UnifiedMessage> command;
    {std::lock_guard<std::mutex> lock(mutex_);if(!commands_.empty()){command=std::move(commands_.front());commands_.pop_front();}}
    UnifiedMessage message;bool isCommand=bool(command);
    if(command)message=std::move(*command);
    else {
        if(now<nextPoll_)return;
        nextPoll_=now+std::chrono::milliseconds(config_.pollMs);
        message.protocol=Protocol::modbus_rtu;message.deviceId="rtu-"+std::to_string(config_.slave);
        message.slave=static_cast<uint8_t>(config_.slave);message.address=config_.registerAddress;
        message.timeout=std::chrono::milliseconds(config_.responseMs);
    }
    serial::ByteBuffer bytes{message.slave,static_cast<uint8_t>(isCommand?6:3),
        static_cast<uint8_t>(message.address>>8U),static_cast<uint8_t>(message.address)};
    if(isCommand)bytes.insert(bytes.end(),message.payload.begin(),message.payload.end());
    else {bytes.push_back(0);bytes.push_back(1);}
    auto deadline=std::min(now+std::chrono::milliseconds(config_.responseMs),message.enqueuedAt+message.timeout);
    pending_=Pending{message,serial::RtuFrameParser::appendCrc(std::move(bytes)),0,deadline,isCommand};
    if(now>=deadline){++rtuTimeouts_;finish(Quality::timeout);return;}
    flush();
}
void SouthboundReactor::received(const serial::ByteBuffer& frame) {
    if(!pending_ || frame[0]!=pending_->message.slave || pending_->offset!=pending_->request.size()) {++rtuUnexpected_;return;}
    unsigned function=pending_->command?6:3;
    if(frame[1]==(function|0x80U) && frame.size()==5){finish(Quality::invalid,frame);return;}
    bool valid=pending_->command ? frame==pending_->request : frame.size()==7 && frame[1]==3 && frame[2]==2;
    if(!valid){++rtuUnexpected_;return;}
    ++rtuReceived_;finish(Quality::good,frame);
}
void SouthboundReactor::run(const std::atomic<bool>& running) {
    epoll_event events[8];
    while(running) {
        tick();
        int count=epoll_wait(epoll_,events,8,10);
        if(count<0){if(errno==EINTR)continue;throw std::runtime_error("southbound epoll_wait");}
        for(int i=0;i<count;++i) {
            int fd=events[i].data.fd;
            if(fd==wake_){uint64_t value;while(read(wake_,&value,sizeof(value))>0){};}
            else if(fd==can_.nativeHandle()) {
                // LT remains armed: budget limits CAN bursts so serial/timers progress.
                for(unsigned budget=0;budget<64;++budget){UnifiedMessage m;if(!can_.receiveReady(m))break;emit_(std::move(m));}
            } else if(serial_ && fd==serial_->nativeHandle()) {
                if(events[i].events&EPOLLIN) {
                    for(unsigned budget=0;budget<16;++budget) {
                        const auto before=serial_->parserMetrics().bytesReceived;
                        for(const auto& frame:serial_->readAvailable())received(frame);
                        const auto& stats=serial_->parserMetrics();
                        parserRejected_=stats.crcCandidatesRejected;parserDiscarded_=stats.bytesDiscarded;parserBuffered_=stats.bufferedBytes;
                        if(stats.bytesReceived==before)break;
                    }
                }
                if(events[i].events&EPOLLOUT)flush();
                if(events[i].events&(EPOLLERR|EPOLLHUP))
                    throw std::runtime_error("RTU endpoint disconnected");
            }
        }
    }
}
void SouthboundReactor::writeMetrics(std::ostream& out) const {
    out<<"{\"responses\":"<<rtuReceived_<<",\"timeouts\":"<<rtuTimeouts_
       <<",\"unexpected\":"<<rtuUnexpected_<<",\"rejected\":"<<rtuRejected_
       <<",\"writes_confirmed\":"<<rtuWrites_<<",\"crc_candidates_rejected\":"<<parserRejected_
       <<",\"bytes_discarded\":"<<parserDiscarded_<<",\"buffered_bytes\":"<<parserBuffered_<<"}";
}
}
