#include "acquisition_scheduler.hpp"
#include <algorithm>
#include <stdexcept>
namespace mqmgateway::edge {
AcquisitionScheduler::AcquisitionScheduler(std::vector<std::shared_ptr<IAcquisitionDriver>> drivers)
    : drivers_(std::move(drivers)) {
    if (drivers_.size() > 256) throw std::invalid_argument("too many scheduled acquisition drivers");
    for (const auto& driver : drivers_)
        if (!driver || driver->interval().count() < 1) throw std::invalid_argument("acquisition interval");
}
void AcquisitionScheduler::start() {
    if (!workers_.empty() || drivers_.empty()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = false;
        due_.clear();
        inFlight_.assign(drivers_.size(), false);
        cursor_ = 0;
        const auto now = std::chrono::steady_clock::now();
        for (const auto& driver : drivers_) due_.push_back(now + driver->interval());
    }
    for (unsigned i = 0; i < 2; ++i) workers_.emplace_back([this] { run(); });
}
void AcquisitionScheduler::stop() noexcept {
    { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
    changed_.notify_all();
    for (auto& worker : workers_) if (worker.joinable()) worker.join();
    workers_.clear();
}
void AcquisitionScheduler::run() {
    using Clock = std::chrono::steady_clock;
    while (true) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (stopping_) break;
        const auto now = Clock::now();
        std::size_t chosen = drivers_.size();
        auto next = Clock::time_point::max();
        for (std::size_t scan = 0; scan < drivers_.size(); ++scan) {
            const auto index = (cursor_ + scan) % drivers_.size();
            if (inFlight_[index]) continue;
            if (due_[index] <= now) { chosen = index; break; }
            next = std::min(next, due_[index]);
        }
        if (chosen == drivers_.size()) {
            if (next == Clock::time_point::max()) changed_.wait(lock);
            else changed_.wait_until(lock, next);
            continue;
        }
        if (now - due_[chosen] > drivers_[chosen]->interval()) ++late_;
        inFlight_[chosen] = true;
        cursor_ = (chosen + 1) % drivers_.size();
        lock.unlock();
        try { drivers_[chosen]->acquire(now); }
        catch (...) { ++errors_; }
        ++runs_;
        lock.lock();
        due_[chosen] = Clock::now() + drivers_[chosen]->interval();
        inFlight_[chosen] = false;
        lock.unlock();
        changed_.notify_all();
    }
}
void AcquisitionScheduler::appendMetrics(std::ostream& out) const {
    out << ",\"acquisition_scheduler\":{\"runs\":" << runs_.load()
        << ",\"late\":" << late_.load() << ",\"errors\":" << errors_.load() << '}';
}
}
