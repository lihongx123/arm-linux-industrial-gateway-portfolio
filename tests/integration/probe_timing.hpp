#pragma once
#include <chrono>
#include <cstdlib>
#include <stdexcept>
// Standalone probes cannot link the Catch2-dependent legacy timing implementation.
namespace probe {
struct timing {
    static double factor() {
        static const double value = [] {
            const char* env = std::getenv("MQM_TEST_TIMING_FACTOR");
            const double result = env ? std::stod(env) : 1;
            if (!(result >= 1 && result <= 100)) throw std::invalid_argument("timing factor 1..100");
            return result;
        }();
        return value;
    }
    static std::chrono::milliseconds milliseconds(int n) { return std::chrono::milliseconds(static_cast<long long>(n * factor())); }
    static std::chrono::milliseconds seconds(int n) { return milliseconds(n * 1000); }
};
}
