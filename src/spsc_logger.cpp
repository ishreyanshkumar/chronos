// ─────────────────────────────────────────────────────────────────────────────
// Chronos :: SPSCLogger implementation
// ─────────────────────────────────────────────────────────────────────────────
#include "spsc_logger.hpp"
#include "cpu_utils.hpp"

#include <chrono>
#include <thread>

namespace chronos {

SPSCLogger::SPSCLogger(const std::string& path)
    : file_(path) {
    if (!file_.is_open())
        throw std::runtime_error("Cannot open trade log: " + path);

    file_ << "buy_id,sell_id,price,quantity,ts_ns\n";
    file_.flush();

    // Start the drain thread.
    worker_ = std::thread([this]{ DrainLoop(); });
}

SPSCLogger::~SPSCLogger() {
    stop_.store(true, std::memory_order_release);
    if (worker_.joinable()) worker_.join();
    file_.flush();
}

bool SPSCLogger::TryLog(const Trade& t) noexcept {
    bool ok = ring_.TryPush(t);
    if (!ok) dropped_.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

void SPSCLogger::Flush() {
    // Spin until the consumer has drained the ring.
    while (!ring_.Empty())
        std::this_thread::yield();
    file_.flush();
}

void SPSCLogger::DrainLoop() {
    // Logger thread does not need a real-time core – keep it off Core 0/1.
    Trade t;
    while (true) {
        bool worked = false;
        while (ring_.TryPop(t)) {
            file_ << t.buy_id  << ','
                  << t.sell_id << ','
                  << t.price   << ','
                  << t.quantity << ','
                  << t.ts_ns   << '\n';
            worked = true;
        }
        if (!worked) {
            if (stop_.load(std::memory_order_acquire)) break;
            // Brief pause avoids burning 100% CPU on an idle logger.
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }
    // Final drain after stop signal.
    while (ring_.TryPop(t)) {
        file_ << t.buy_id  << ','
              << t.sell_id << ','
              << t.price   << ','
              << t.quantity << ','
              << t.ts_ns   << '\n';
    }
}

} // namespace chronos
