#pragma once
#include "board_driver.hpp"
namespace mqmgateway::board {
struct SpiConfig { BoardConfig point; unsigned mode{0}, speedHz{1000000}, bits{8}; std::vector<std::uint8_t> command; };
struct I2cConfig { BoardConfig point; unsigned address{0}; std::vector<std::uint8_t> registerBytes; };
struct GpioConfig { BoardConfig point; unsigned line{0}; bool activeLow{false}, output{false}; };
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
}
