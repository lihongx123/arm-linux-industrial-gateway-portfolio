#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <utility>

namespace mqmgateway::edge {

// Wall time is for evidence; all deadlines use monotonic time.
class TimeService {
public:
    using Monotonic = std::chrono::steady_clock;
    using Wall = std::chrono::system_clock;
    using MonotonicNow = std::function<Monotonic::time_point()>;
    using WallNow = std::function<Wall::time_point()>;

    explicit TimeService(MonotonicNow monotonic = [] { return Monotonic::now(); },
                         WallNow wall = [] { return Wall::now(); })
        : monotonic_(std::move(monotonic)), wall_(std::move(wall)) {}

    Monotonic::time_point monotonic() const { return monotonic_(); }
    Wall::time_point wall() const { return wall_(); }
    std::int64_t epochMs() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(wall().time_since_epoch()).count();
    }

private:
    MonotonicNow monotonic_;
    WallNow wall_;
};

}  // namespace mqmgateway::edge
