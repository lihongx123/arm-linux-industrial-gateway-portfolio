#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <ostream>
#include <unordered_map>

namespace mqmgateway::iot {

// Fixed-space logarithmic histogram. Quantiles are bucket UPPER bounds, not
// interpolated measurements. The exact maximum is also exposed.
class StageLatency {
public:
    void observe(std::chrono::steady_clock::duration elapsed) {
        const double us = std::max(0.0, std::chrono::duration<double, std::micro>(elapsed).count());
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t bin = 0;
        double upper = 1.0;
        while (us > upper && bin + 1 < buckets_.size()) { ++bin; upper *= 2; }
        ++buckets_[bin];
        ++count_;
        sum_ += us;
        max_ = std::max(max_, us);
    }

    void write(std::ostream& out) const {
        std::lock_guard<std::mutex> lock(mutex_);
        out << "{\"count\":" << count_ << ",\"mean_us\":" << (count_ ? sum_ / count_ : 0)
            << ",\"max_us\":" << max_ << ",\"p50_upper_us\":" << percentile(50)
            << ",\"p95_upper_us\":" << percentile(95)
            << ",\"p99_upper_us\":" << percentile(99) << '}';
    }

private:
    double percentile(std::uint64_t percent) const {
        if (!count_) return 0;
        const auto wanted = count_ / 100 * percent + (count_ % 100 * percent + 99) / 100;
        std::uint64_t cumulative = 0;
        double upper = 1;
        for (auto bucket : buckets_) {
            cumulative += bucket;
            if (cumulative >= wanted) return std::min(upper, max_);
            upper *= 2;
        }
        return max_;
    }
    mutable std::mutex mutex_;
    std::array<std::uint64_t, 64> buckets_{};
    std::uint64_t count_{0};
    double sum_{0}, max_{0};
};

// Diagnostic MID correlation is bounded independently of libmosquitto's queue.
// It does NOT implement flow control or expose the actual wire inflight window.
class PublishTracker {
public:
    explicit PublishTracker(std::size_t capacity = 4096) : capacity_(capacity) {}

    template <typename Send>
    int submit(int qos, bool telemetry, Send send) {
        // QoS 0 callbacks may run synchronously inside publish. A recursive lock
        // plus early-completion capture also closes the register/ACK race.
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        const auto start = std::chrono::steady_clock::now();
        ++calls_;
        submitting_ = true;
        early_.reset();
        int mid = 0;
        const int result = send(&mid);
        submitting_ = false;
        apiLatency_.observe(std::chrono::steady_clock::now() - start);
        if (result != 0) {
            ++errors_;
            if (early_) ++unknown_;
            return result;
        }
        ++accepted_;
        if (telemetry) ++telemetryAccepted_;
        const Entry entry{start, qos, telemetry};
        if (early_ && early_->first == mid) {
            finish(entry, early_->second);
        } else if (pending_.count(mid)) {
            // MID reuse while still pending makes correlation ambiguous.
            pending_.erase(mid);
            dropped_ += 2;
        } else if (pending_.size() >= capacity_) {
            ++dropped_;
        } else {
            pending_.emplace(mid, entry);
            peak_ = std::max(peak_, pending_.size());
        }
        return result;
    }

    void completed(int mid) {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        const auto it = pending_.find(mid);
        if (it != pending_.end()) {
            finish(it->second, now);
            pending_.erase(it);
        } else if (submitting_) {
            early_ = std::make_pair(mid, now);
        } else {
            ++unknown_;
        }
    }

    void write(std::ostream& out) const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        double oldestUs = 0;
        const auto now = std::chrono::steady_clock::now();
        for (const auto& item : pending_) {
            oldestUs = std::max(oldestUs, std::chrono::duration<double, std::micro>(now - item.second.start).count());
        }
        out << "{\"publish_calls\":" << calls_ << ",\"publish_accepted\":" << accepted_
            << ",\"publish_api_errors\":" << errors_ << ",\"puback_received\":" << acked_
            << ",\"qos0_local_completed\":" << qos0Completed_
            << ",\"telemetry_accepted\":" << telemetryAccepted_
            << ",\"telemetry_puback_received\":" << telemetryAcked_
            << ",\"pending_tracked\":" << pending_.size() << ",\"pending_peak\":" << peak_
            << ",\"oldest_pending_us\":" << oldestUs
            << ",\"tracking_dropped\":" << dropped_ << ",\"unmatched_callbacks\":" << unknown_
            << ",\"tracking_complete\":" << ((dropped_ == 0 && unknown_ == 0) ? "true" : "false")
            << ",\"publish_api\":";
        apiLatency_.write(out);
        out << ",\"telemetry_puback\":";
        ackLatency_.write(out);
        out << '}';
    }

private:
    using Clock = std::chrono::steady_clock;
    struct Entry { Clock::time_point start; int qos; bool telemetry; };
    void finish(const Entry& entry, Clock::time_point now) {
        if (entry.qos == 1) {
            ++acked_;
            if (entry.telemetry) { ++telemetryAcked_; ackLatency_.observe(now - entry.start); }
        } else {
            ++qos0Completed_;
        }
    }
    const std::size_t capacity_;
    mutable std::recursive_mutex mutex_;
    std::unordered_map<int, Entry> pending_;
    std::optional<std::pair<int, Clock::time_point>> early_;
    bool submitting_{false};
    std::size_t peak_{0};
    std::uint64_t calls_{0}, accepted_{0}, errors_{0}, acked_{0}, qos0Completed_{0};
    std::uint64_t telemetryAccepted_{0}, telemetryAcked_{0}, dropped_{0}, unknown_{0};
    StageLatency apiLatency_, ackLatency_;
};

}  // namespace mqmgateway::iot
