#include "board_driver.hpp"
#include "linux_backends.hpp"
#include "raw_uart_driver.hpp"
#include "acquisition_scheduler.hpp"
#include "gateway_core.hpp"
#include "southbound_reactor.hpp"
#include "serial_port.hpp"
#include "can_driver.hpp"
#include "modbus_rtu_driver.hpp"
#include "modbus_tcp_driver.hpp"
#include "generic_tcp_driver.hpp"
#ifdef PHASE5_MIXED
#include "mc_driver.hpp"
#endif
#ifdef PHASE5_FULL_MIXED
#include "opcua_driver.hpp"
#include "s7_driver.hpp"
#include <open62541/server.h>
#include <open62541/server_config_default.h>
#include <snap7.h>
#endif
#include "probe_timing.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <linux/can.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>
using namespace mqmgateway;
namespace {
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
class FakeBackend final : public board::IBoardBackend {
public:
    std::atomic<bool> fail{false}, shortRead{false};
    std::atomic<unsigned> reads{0}, writes{0}, readDelayMs{0};
    std::atomic<bool> slowActive{false};
    std::vector<std::uint8_t> value{0, 42};
    void open() override {}
    void close() noexcept override {}
    std::vector<std::uint8_t> read() override {
        ++reads;
        if (const auto delay = readDelayMs.load()) {
            slowActive = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            slowActive = false;
        }
        if (fail) throw std::runtime_error("injected I/O error");
        return shortRead ? std::vector<std::uint8_t>{0} : value;
    }
    void write(const std::vector<std::uint8_t>& bytes) override {
        if (fail) throw std::runtime_error("injected write error");
        ++writes; value = bytes;
    }
};
struct TcpServer {
    int listener{-1}, peer{-1};
    unsigned port{0};
    TcpServer() {
        listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        require(listener >= 0, "TCP socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "TCP bind");
        socklen_t length = sizeof(address);
        require(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0, "TCP port");
        port = ntohs(address.sin_port);
        require(listen(listener, 2) == 0, "TCP listen");
    }
    ~TcpServer() { if (peer >= 0) close(peer); if (listener >= 0) close(listener); }
    void waitRead(int fd) {
        pollfd ready{fd, POLLIN, 0};
        require(poll(&ready, 1, probe::timing::seconds(5).count()) > 0, "TCP read timeout");
    }
    void acceptPeer() {
        waitRead(listener);
        peer = accept(listener, nullptr, nullptr);
        require(peer >= 0, "TCP accept");
    }
    std::vector<std::uint8_t> receive(std::size_t size) {
        std::vector<std::uint8_t> bytes(size);
        std::size_t offset = 0;
        while (offset < size) {
            waitRead(peer);
            const auto count = recv(peer, bytes.data() + offset, size - offset, 0);
            require(count > 0, "TCP receive");
            offset += count;
        }
        return bytes;
    }
    void sendBytes(const std::vector<std::uint8_t>& bytes) {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto count = send(peer, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
            require(count > 0, "TCP send");
            offset += count;
        }
    }
};
#ifdef PHASE5_FULL_MIXED
struct OpcServer {
    UA_Server* server{nullptr};
    std::atomic<bool> running{false};
    std::thread thread;
    OpcServer() {
        server = UA_Server_new();
        require(server != nullptr, "mixed OPC UA allocation");
        require(UA_ServerConfig_setDefault(UA_Server_getConfig(server)) == UA_STATUSCODE_GOOD,
                "mixed OPC UA config");
        UA_VariableAttributes attr = UA_VariableAttributes_default;
        UA_Int32 value = 42;
        UA_Variant_setScalar(&attr.value, &value, &UA_TYPES[UA_TYPES_INT32]);
        attr.dataType = UA_TYPES[UA_TYPES_INT32].typeId;
        attr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
        char nodeName[] = "mixed.answer";
        char browseName[] = "mixed answer";
        require(UA_Server_addVariableNode(server, UA_NODEID_STRING(1, nodeName),
                UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
                UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES), UA_QUALIFIEDNAME(1, browseName),
                UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), attr, nullptr, nullptr)
                == UA_STATUSCODE_GOOD, "mixed OPC UA variable");
        require(UA_Server_run_startup(server) == UA_STATUSCODE_GOOD, "mixed OPC UA startup");
        running = true;
        thread = std::thread([this] { while (running) UA_Server_run_iterate(server, true); });
    }
    ~OpcServer() {
        running = false;
        if (thread.joinable()) thread.join();
        if (server) { UA_Server_run_shutdown(server); UA_Server_delete(server); }
    }
};
struct S7Server {
    TS7Server server;
    std::uint16_t port{0};
    std::uint8_t db1[2]{0, 42};
    bool started{false};
    S7Server() {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        require(fd >= 0, "mixed S7 port socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        const int bound = bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        socklen_t length = sizeof(address);
        const int named = bound == 0 ? getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) : -1;
        close(fd);
        require(bound == 0 && named == 0, "mixed S7 port allocation");
        port = ntohs(address.sin_port);
        require(server.SetParam(p_u16_LocalPort, &port) == 0, "mixed S7 port config");
        require(server.RegisterArea(srvAreaDB, 1, db1, sizeof(db1)) == 0, "mixed S7 DB");
        require(server.StartTo("127.0.0.1") == 0, "mixed S7 server start");
        started = true;
    }
    ~S7Server() { if (started) server.Stop(); }
};
#endif
struct UartLink {
    std::string directory, path;
    explicit UartLink(const char* target) {
        char pattern[] = "/tmp/mqmgateway-uart-XXXXXX";
        const auto* created = mkdtemp(pattern);
        require(created != nullptr, "UART link directory");
        directory = created; path = directory + "/device";
        require(symlink(target, path.c_str()) == 0, "UART link");
    }
    void retarget(const char* target) {
        require(unlink(path.c_str()) == 0 && symlink(target, path.c_str()) == 0, "UART link retarget");
    }
    ~UartLink() { unlink(path.c_str()); rmdir(directory.c_str()); }
};
void injectCan(unsigned address, std::initializer_list<std::uint8_t> data) {
    const int fd = socket(PF_CAN, SOCK_RAW | SOCK_CLOEXEC, CAN_RAW);
    require(fd >= 0, "CAN socket");
    ifreq request{};
    std::strncpy(request.ifr_name, "vcan0", IFNAMSIZ - 1);
    require(ioctl(fd, SIOCGIFINDEX, &request) == 0, "vcan0 interface");
    sockaddr_can location{};
    location.can_family = AF_CAN; location.can_ifindex = request.ifr_ifindex;
    require(bind(fd, reinterpret_cast<sockaddr*>(&location), sizeof(location)) == 0, "CAN bind");
    can_frame frame{};
    frame.can_id = address;
    frame.can_dlc = data.size();
    std::copy(data.begin(), data.end(), frame.data);
    require(write(fd, &frame, sizeof(frame)) == sizeof(frame), "CAN inject");
    close(fd);
}
}
int main() {
    try {
        TcpServer modbusTcp, genericTcp;
#ifdef PHASE5_MIXED
        TcpServer mcTcp;
#endif
#ifdef PHASE5_FULL_MIXED
        OpcServer opcServer;
        S7Server s7Server;
#endif
        board::BoardConfig boardConfig;
        boardConfig.deviceId = "adc01"; boardConfig.pointId = "channel0";
        boardConfig.readLength = 2; boardConfig.intervalMs = 30;
        boardConfig.type = edge::PointValueType::integer; boardConfig.scale = .5; boardConfig.offset = -10;
        boardConfig.writable = true;
        auto spiFake = std::make_shared<FakeBackend>();
        auto i2cFake = std::make_shared<FakeBackend>();
        auto gpioFake = std::make_shared<FakeBackend>();
        gpioFake->value = {1};
        std::mutex mutex; std::condition_variable changed;
        std::vector<edge::UnifiedMessageV2> out;
        edge::GatewayCore core([&](auto m) {
            std::lock_guard<std::mutex> lock(mutex);
            out.push_back(std::move(m)); changed.notify_all();
        });
        const auto add = [&](const std::string& kind, board::BoardConfig cfg, std::shared_ptr<FakeBackend> backend) {
            auto d = std::make_shared<board::BoardDriver>(kind, cfg, backend);
            require(core.addDriver(d) && core.addDevice({cfg.deviceId, d->id()}) &&
                    core.addPoint(*d->describePoint({})), "board registration");
        };
        add("spi", boardConfig, spiFake);
        boardConfig.deviceId = "temp01"; boardConfig.pointId = "temperature";
        add("i2c", boardConfig, i2cFake);
        boardConfig.deviceId = "door01"; boardConfig.pointId = "open";
        boardConfig.readLength = 1; boardConfig.type = edge::PointValueType::boolean;
        boardConfig.scale = 1; boardConfig.offset = 0;
        add("gpio", boardConfig, gpioFake);
        int master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
        require(master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0, "PTY setup");
        UartLink uartLink(ptsname(master));
        drivers::RawUartConfig uart;
        uart.deviceId = "serial_sensor01"; uart.pointId = "value";
        uart.serial.path = uartLink.path; uart.reconnectMs = 50;
        auto serial = std::make_shared<drivers::RawUartDriver>(uart, 8);
        require(core.addDriver(serial) && core.addDevice({uart.deviceId, serial->id()}) &&
                core.addPoint(*serial->describePoint({})), "UART registration");
        const int rtuMaster = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
        require(rtuMaster >= 0 && grantpt(rtuMaster) == 0 && unlockpt(rtuMaster) == 0, "RTU PTY setup");
        drivers::RtuConfig rtuConfig;
        rtuConfig.device = ptsname(rtuMaster);
        rtuConfig.pollMs = 30;
        rtuConfig.responseMs = 300;
        auto rtu = std::make_shared<drivers::ModbusRtuDriver>(rtuConfig, 8);
        require(core.addDriver(rtu) && core.addDevice({"rtu-1", rtu->id()}), "RTU registration");
        drivers::ModbusTcpConfig mtConfig;
        mtConfig.deviceId = "plc01"; mtConfig.address = "127.0.0.1"; mtConfig.port = modbusTcp.port;
        mtConfig.pollMs = 50; mtConfig.responseMs = 300; mtConfig.reconnectMs = 50;
        auto mt = std::make_shared<drivers::ModbusTcpDriver>(mtConfig, 8);
        require(core.addDriver(mt) && core.addDevice({mtConfig.deviceId, mt->id()}), "Modbus TCP registration");
        drivers::TcpConfig gtConfig;
        gtConfig.deviceId = "tcp01"; gtConfig.address = "127.0.0.1"; gtConfig.port = genericTcp.port;
        gtConfig.reconnectMs = 50;
        auto gt = std::make_shared<drivers::GenericTcpDriver>(gtConfig, 8);
        require(core.addDriver(gt) && core.addDevice({gtConfig.deviceId, gt->id()}), "Generic TCP registration");
#ifdef PHASE5_MIXED
        drivers::McConfig mcConfig;
        mcConfig.deviceId = "mc01"; mcConfig.pointId = "speed";
        mcConfig.address = "127.0.0.1"; mcConfig.port = mcTcp.port;
        mcConfig.registerAddress = 7; mcConfig.pollMs = 50;
        mcConfig.responseMs = 300; mcConfig.reconnectMs = 50;
        auto mc = std::make_shared<drivers::McDriver>(mcConfig, 8);
        require(core.addDriver(mc) && core.addDevice({mcConfig.deviceId, mc->id()}) &&
                core.addPoint({mcConfig.deviceId, mcConfig.pointId, edge::PointValueType::integer,
                               "", true, mcConfig.registerAddress}), "MC registration");
#endif
#ifdef PHASE5_FULL_MIXED
        drivers::OpcUaConfig opcConfig;
        opcConfig.deviceId = "opcua01"; opcConfig.pointId = "answer";
        opcConfig.endpoint = "opc.tcp://127.0.0.1:4840";
        opcConfig.nodeId = "ns=1;s=mixed.answer";
        opcConfig.writable = true; opcConfig.intervalMs = 50; opcConfig.responseMs = 500;
        auto opc = std::make_shared<drivers::OpcUaDriver>(opcConfig);
        require(core.addDriver(opc) && core.addDevice({opcConfig.deviceId, opc->id()}) &&
                core.addPoint({opcConfig.deviceId, opcConfig.pointId, edge::PointValueType::integer,
                               opcConfig.nodeId, true, 0}), "mixed OPC UA registration");
        drivers::S7Config s7Config;
        s7Config.deviceId = "s701"; s7Config.pointId = "db_word";
        s7Config.address = "127.0.0.1"; s7Config.port = s7Server.port;
        s7Config.dbNumber = 1; s7Config.byteOffset = 0;
        s7Config.intervalMs = 50; s7Config.writable = true;
        auto s7 = std::make_shared<drivers::S7Driver>(s7Config);
        require(core.addDriver(s7) && core.addDevice({s7Config.deviceId, s7->id()}) &&
                core.addPoint({s7Config.deviceId, s7Config.pointId, edge::PointValueType::integer,
                               "DB1.DBW0", true, 0}), "mixed S7 registration");
#endif
        auto can = std::make_shared<drivers::CanDriver>("vcan0", false);
        require(core.addDriver(can) && core.addDevice({"can-291", can->id()}), "CAN registration");
        require(core.start(), "core start");
        iot::SouthboundReactor reactor(core.eventDrivers());
        std::atomic<bool> running{true};
        std::exception_ptr reactorError;
        std::thread rx([&] {
            try { reactor.run(running); }
            catch (...) { std::lock_guard<std::mutex> lock(mutex); reactorError = std::current_exception(); changed.notify_all(); }
        });
        edge::AcquisitionScheduler scheduler(core.acquisitionDrivers());
        scheduler.start();
        struct Cleanup {
            edge::AcquisitionScheduler& scheduler;
            std::atomic<bool>& running;
            iot::SouthboundReactor& reactor;
            std::thread& rx;
            edge::GatewayCore& core;
            int& master;
            int rtuMaster;
            ~Cleanup() {
                scheduler.stop(); running = false; reactor.wake();
                if (rx.joinable()) rx.join();
                core.stop(); if (master >= 0) close(master); close(rtuMaster);
            }
        } cleanup{scheduler, running, reactor, rx, core, master, rtuMaster};
        const auto await = [&](const std::string& device, const std::string& point, const std::string& value) {
            std::unique_lock<std::mutex> lock(mutex);
            return changed.wait_for(lock, probe::timing::seconds(5), [&] {
                if (reactorError) std::rethrow_exception(reactorError);
                for (const auto& m : out) if (m.deviceId == device && m.pointId == point &&
                    m.dataType == iot::DataType::telemetry && m.cookedValue == value) return true;
                return false;
            });
        };
        require(await("adc01", "channel0", "11"), "SPI scaled point");
        require(await("temp01", "temperature", "11"), "I2C scaled point");
        require(await("door01", "open", "true"), "GPIO boolean point");
        require(write(master, "AB", 2) == 2 && write(master, "C\nD\n", 4) == 4, "UART fragmented write");
        require(await("serial_sensor01", "value", "414243"), "UART fragmented frame");
        require(await("serial_sensor01", "value", "44"), "UART coalesced frame");
        std::vector<std::uint8_t> oversized(4097, 'X');
        oversized.push_back('\n'); oversized.push_back('Z'); oversized.push_back('\n');
        std::size_t offset = 0;
        while (offset < oversized.size()) {
            pollfd writable{master, POLLOUT, 0};
            require(poll(&writable, 1, probe::timing::seconds(5).count()) > 0, "UART oversized write wait");
            const auto n = write(master, oversized.data() + offset, oversized.size() - offset);
            require(n > 0, "UART oversized write");
            offset += n;
        }
        require(await("serial_sensor01", "value", "5a"), "UART recovery after oversized frame");
        {
            std::lock_guard<std::mutex> lock(mutex);
            unsigned uartFrames = 0;
            for (const auto& m : out)
                if (m.deviceId == "serial_sensor01" && m.dataType == iot::DataType::telemetry) ++uartFrames;
            require(uartFrames == 3, "oversized UART frame must be discarded completely");
        }
        modbusTcp.acceptPeer();
        genericTcp.acceptPeer();
#ifdef PHASE5_MIXED
        mcTcp.acceptPeer();
#endif
        auto mbRequest = modbusTcp.receive(12);
        std::vector<std::uint8_t> mbResponse(mbRequest.begin(), mbRequest.begin() + 7);
        mbResponse[5] = 5;
        mbResponse.insert(mbResponse.end(), {3, 2, 0, 42});
        modbusTcp.sendBytes(mbResponse);
        require(await("plc01", "holding-0", "42"), "Modbus TCP mapped point");
        genericTcp.sendBytes({0, 2, 1, 2});
        require(await("tcp01", "payload", "0102"), "Generic TCP mapped point");
#ifdef PHASE5_MIXED
        auto mcRequest = mcTcp.receive(21);
        require(mcRequest[0] == 0x50 && mcRequest[11] == 1 && mcRequest[12] == 4 &&
                mcRequest[18] == 0xA8, "MC read in mixed core");
        mcTcp.sendBytes({0xD0,0,0,0xFF,0xFF,3,0,4,0,0,0,0x34,0x12});
        require(await("mc01", "speed", "4660"), "MC mapped point in mixed core");
        edge::UnifiedMessageV2 mcWrite;
        mcWrite.deviceId = "mc01"; mcWrite.dataType = iot::DataType::command;
        mcWrite.legacyAddress = 7; mcWrite.rawPayload = {0x12,0x34};
        require(core.submit(mcWrite), "MC write in mixed core");
        for (unsigned i = 0; i < 20; ++i) {
            mcRequest = mcTcp.receive(21);
            if (mcRequest[12] == 0x14) {
                const auto data = mcTcp.receive(2);
                require(data == std::vector<std::uint8_t>({0x34,0x12}), "MC mixed write encoding");
                break;
            }
            mcTcp.sendBytes({0xD0,0,0,0xFF,0xFF,3,0,4,0,0,0,0x34,0x12});
        }
        require(mcRequest[12] == 0x14, "MC mixed write dispatch");
        mcTcp.sendBytes({0xD0,0,0,0xFF,0xFF,3,0,2,0,0,0});
        {
            std::unique_lock<std::mutex> lock(mutex);
            require(changed.wait_for(lock, probe::timing::seconds(5), [&] {
                for (const auto& m : out)
                    if (m.deviceId == "mc01" && m.pointId == "speed" &&
                        m.dataType == iot::DataType::status && m.status == "success") return true;
                return false;
            }), "MC mixed command result");
        }
#endif
#ifdef PHASE5_FULL_MIXED
        require(await("opcua01", "answer", "42"), "OPC UA mapped point in mixed core");
        require(await("s701", "db_word", "42"), "S7 mapped point in mixed core");
        edge::UnifiedMessageV2 opcWrite;
        opcWrite.deviceId = "opcua01"; opcWrite.pointId = "answer";
        opcWrite.dataType = iot::DataType::command; opcWrite.rawPayload = {0, 0, 0, 77};
        require(core.submit(opcWrite), "OPC UA mixed write dispatch");
        edge::UnifiedMessageV2 s7Write;
        s7Write.deviceId = "s701"; s7Write.pointId = "db_word";
        s7Write.dataType = iot::DataType::command; s7Write.rawPayload = {0, 88};
        require(core.submit(s7Write), "S7 mixed write dispatch");
        const auto awaitStatus = [&](const std::string& device) {
            std::unique_lock<std::mutex> lock(mutex);
            return changed.wait_for(lock, probe::timing::seconds(5), [&] {
                for (const auto& m : out)
                    if (m.deviceId == device && m.dataType == iot::DataType::status &&
                        m.status == "success") return true;
                return false;
            });
        };
        require(awaitStatus("opcua01") && awaitStatus("s701"), "mixed OPC UA/S7 write acknowledgements");
        require(await("opcua01", "answer", "77"), "mixed OPC UA read after write");
        require(await("s701", "db_word", "88"), "mixed S7 read after write");
#endif
        pollfd rtuReady{rtuMaster, POLLIN, 0};
        require(poll(&rtuReady, 1, probe::timing::seconds(5).count()) > 0, "RTU poll request");
        std::uint8_t rtuRequest[8];
        require(read(rtuMaster, rtuRequest, sizeof(rtuRequest)) == 8 && rtuRequest[1] == 3,
                "RTU request frame");
        const auto rtuResponse = serial::RtuFrameParser::appendCrc({1, 3, 2, 0, 42});
        require(write(rtuMaster, rtuResponse.data(), rtuResponse.size()) == static_cast<ssize_t>(rtuResponse.size()),
                "RTU response write");
        require(await("rtu-1", "holding-0", "42"), "RTU mapped point");
        injectCan(291, {1, 2, 3});
        require(await("can-291", "frame", "010203"), "CAN mapped point");
        for (const auto* device : {"adc01", "temp01", "door01", "serial_sensor01",
                                   "rtu-1", "plc01", "tcp01", "can-291"}) {
            const auto health = core.deviceHealth(device);
            require(health && health->state == edge::HealthState::online,
                    "mixed core device health after mapped telemetry");
        }
#ifdef PHASE5_MIXED
        require(core.deviceHealth("mc01")->state == edge::HealthState::online,
                "mixed MC device health");
#endif
#ifdef PHASE5_FULL_MIXED
        require(core.deviceHealth("opcua01")->state == edge::HealthState::online &&
                core.deviceHealth("s701")->state == edge::HealthState::online,
                "mixed OPC UA and S7 device health");
#endif
        core.reportQueueStall(true);
        require(core.acknowledgeAlarm("system/queue_stalled"), "mixed alarm acknowledgement");
        core.reportQueueStall(false);
        edge::UnifiedMessageV2 command;
        command.deviceId = "adc01"; command.dataType = iot::DataType::command;
        command.rawPayload = {0, 44};
        require(core.submit(command) && spiFake->writes == 1, "SPI command");
        command.deviceId = "door01"; command.pointId.clear(); command.rawPayload = {0};
        require(core.submit(command) && gpioFake->writes == 1, "GPIO command");
        command.deviceId = "serial_sensor01"; command.pointId.clear(); command.rawPayload = {1, 2};
        require(core.submit(command), "UART command");
        pollfd ready{master, POLLIN, 0};
        require(poll(&ready, 1, probe::timing::seconds(5).count()) > 0, "UART outbound");
        std::uint8_t sent[2];
        require(read(master, sent, 2) == 2 && sent[0] == 1 && sent[1] == 2, "UART write bytes");
        spiFake->shortRead = true;
        const auto beforeErrors = spiFake->reads.load();
        const auto until = std::chrono::steady_clock::now() + probe::timing::seconds(5);
        while (spiFake->reads == beforeErrors && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(probe::timing::milliseconds(5));
        require(spiFake->reads > beforeErrors, "short read scheduled");
        spiFake->fail = true;
        const auto beforeI2c = i2cFake->reads.load();
        spiFake->readDelayMs = 600;
        const auto until2 = std::chrono::steady_clock::now() + probe::timing::seconds(5);
        while (!spiFake->slowActive && std::chrono::steady_clock::now() < until2)
            std::this_thread::sleep_for(probe::timing::milliseconds(5));
        require(spiFake->slowActive, "slow SPI acquisition started");
        const auto i2cDuringSlow = i2cFake->reads.load();
        const auto fairDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        while (i2cFake->reads == i2cDuringSlow && std::chrono::steady_clock::now() < fairDeadline)
            std::this_thread::sleep_for(probe::timing::milliseconds(5));
        require(i2cFake->reads > i2cDuringSlow, "slow SPI must not starve I2C");
        require(i2cFake->reads > beforeI2c, "I2C still polled");
        spiFake->readDelayMs = 0;
        bool invalid = false;
        try { board::SpiBackend wrong({boardConfig, 4, 1000000, 8, {}}); } catch (...) { invalid = true; }
        require(invalid, "SPI mode validation");
        invalid = false;
        try { board::SpiBackend wrong({boardConfig, 0, 0, 8, {}}); } catch (...) { invalid = true; }
        require(invalid, "SPI speed validation");
        invalid = false;
        try { board::SpiBackend wrong({boardConfig, 0, 1000000, 16, {}}); } catch (...) { invalid = true; }
        require(invalid, "SPI word width validation");
        invalid = false;
        try { board::I2cBackend wrong({boardConfig, 0x80, {}}); } catch (...) { invalid = true; }
        require(invalid, "I2C address validation");
        invalid = false;
        try { board::I2cBackend wrong({boardConfig, 0x40, std::vector<std::uint8_t>(17)}); }
        catch (...) { invalid = true; }
        require(invalid, "I2C register command bound");
        invalid = false;
        try { auto bad = boardConfig; bad.intervalMs = 0;
              board::BoardDriver wrong("spi", bad, std::make_shared<FakeBackend>()); }
        catch (...) { invalid = true; }
        require(invalid, "board polling interval validation");
        auto rs = serial::SerialPort::rs485State({true, true, false, 2, 3});
        require((rs.flags & SER_RS485_ENABLED) && rs.delay_rts_before_send == 2 &&
                rs.delay_rts_after_send == 3, "RS485 ioctl state");
        invalid = false;
        try { serial::SerialPort::rs485State({true, true, false, 1001, 0}); }
        catch (...) { invalid = true; }
        require(invalid, "RS485 delay validation");
        invalid = false;
        try { serial::SerialPort port({uart.serial.path, 115200, 8, 1, 'N', {true}});
              port.open(); } catch (...) { invalid = true; }
        require(invalid, "PTY reports unsupported RS485 ioctl");
        close(master); master = -1;
        master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
        require(master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0, "UART replacement PTY");
        uartLink.retarget(ptsname(master));
        const auto reconnectDeadline = std::chrono::steady_clock::now() + probe::timing::seconds(5);
        bool reopened = false;
        while (std::chrono::steady_clock::now() < reconnectDeadline) {
            std::ostringstream metrics; serial->appendMetrics(metrics);
            if (metrics.str().find("\"reopens\":2") != std::string::npos) { reopened = true; break; }
            std::this_thread::sleep_for(probe::timing::milliseconds(10));
        }
        require(reopened, "UART reconnect after PTY replacement");
        require(write(master, "R\n", 2) == 2, "UART post-reconnect input");
        require(await("serial_sensor01", "value", "52"), "UART post-reconnect mapped frame");
        rusage usage{};
        require(getrusage(RUSAGE_SELF, &usage) == 0, "resource usage");
        const auto cpuMs = usage.ru_utime.tv_sec * 1000L + usage.ru_utime.tv_usec / 1000L +
                           usage.ru_stime.tv_sec * 1000L + usage.ru_stime.tv_usec / 1000L;
        std::cout << "{\"result\":\"PASS\",\"label\":\"MIXED_SOFTWARE_INTEGRATION\",\"devices\":"
#ifdef PHASE5_FULL_MIXED
                  << 11
#elif defined(PHASE5_MIXED)
                  << 9
#else
                  << 8
#endif
                  << ",\"peak_rss_kib\":" << usage.ru_maxrss << ",\"process_cpu_ms\":" << cpuMs;
        core.appendMetrics(std::cout); scheduler.appendMetrics(std::cout); std::cout << "}\n";
    } catch (const std::exception& e) {
        std::cerr << "phase4 software FAIL: " << e.what() << '\n'; return 1;
    }
}
