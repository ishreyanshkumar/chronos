#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// Chronos :: SPSCLogger
//
// Asynchronous trade-execution logger.
//
// Architecture:
//   Matching engine (Core 2) → TryLog(Trade) → SPSCRing → Logger thread
//
// The matching engine never blocks on I/O.  Trades are written into the wait-
// free ring buffer and a dedicated background thread drains them to a CSV file.
// ─────────────────────────────────────────────────────────────────────────────
#include "order.hpp"
#include "spsc_ring.hpp"

#include <atomic>
#include <thread>
#include <string>
#include <fstream>
#include <cstdint>

namespace chronos {

class SPSCLogger {
public:
    static constexpr std::size_t kRingCap = 1 << 16; // 65536 pending trades

    explicit SPSCLogger(const std::string& path);
    ~SPSCLogger();

    // Non-blocking – call from the matching engine core.
    // Returns false if the ring is full (rare, indicates a slow disk).
    [[nodiscard]] bool TryLog(const Trade& t) noexcept;

    [[nodiscard]] uint64_t Dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

    void Flush();  // Block until the ring is fully drained.

private:
    void DrainLoop();

    SPSCRing<Trade, kRingCap>     ring_;
    std::ofstream                 file_;
    std::thread                   worker_;
    std::atomic<bool>             stop_  {false};
    std::atomic<uint64_t>         dropped_ {0};
};

} // namespace chronos
