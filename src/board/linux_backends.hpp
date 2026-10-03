#pragma once
#include "board_driver.hpp"
namespace mqmgateway::board {
struct SpiConfig { BoardConfig point; unsigned mode{0}, speedHz{1000000}, bits{8}; std::vector<std::uint8_t> command; };
struct I2cConfig { BoardConfig point; unsigned address{0}; std::vector<std::uint8_t> registerBytes; };
struct GpioConfig { BoardConfig point; unsigned line{0}; bool activeLow{false}, output{false}; };
struct AdcConfig { BoardConfig point; bool simulation{false}; };
struct PwmConfig { BoardConfig point; std::uint64_t periodNs{0}; bool simulation{false}; };
class SpiBackend final : public IBoardBackend {
public:
    explicit SpiBackend(SpiConfig config);
    void open() override; void close() noexcept override;
    std::vector<std::uint8_t> read() override; void write(const std::vector<std::uint8_t>&) override;
private: SpiConfig mConfig; int mFd{-1};
};
class I2cBackend final : public IBoardBackend {
public:
    explicit I2cBackend(I2cConfig config);
    void open() override; void close() noexcept override;
    std::vector<std::uint8_t> read() override; void write(const std::vector<std::uint8_t>&) override;
private: I2cConfig mConfig; int mFd{-1};
};
class GpioBackend final : public IBoardBackend {
public:
    explicit GpioBackend(GpioConfig config);
    void open() override; void close() noexcept override;
    std::vector<std::uint8_t> read() override; void write(const std::vector<std::uint8_t>&) override;
private: GpioConfig mConfig; int mChip{-1}, mLine{-1};
};
class AdcBackend final : public IBoardBackend {
public:
    explicit AdcBackend(AdcConfig config);
    void open() override; void close() noexcept override;
    std::vector<std::uint8_t> read() override; void write(const std::vector<std::uint8_t>&) override;
private: AdcConfig mConfig; bool mOpen{false};
};
class PwmBackend final : public IBoardBackend {
public:
    explicit PwmBackend(PwmConfig config);
    void open() override; void close() noexcept override;
    std::vector<std::uint8_t> read() override; void write(const std::vector<std::uint8_t>&) override;
private: PwmConfig mConfig; bool mOpen{false};
};
}
