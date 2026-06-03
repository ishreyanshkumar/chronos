#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// Chronos :: SPSCRing<T, N>
//
// Wait-free, lock-free Single-Producer Single-Consumer ring buffer.
//
// Implementation notes:
//   • head_ and tail_ are on separate cache lines to eliminate false sharing
//     between the producer and consumer cores.
//   • All loads/stores use std::atomic with relaxed or acquire/release
//     semantics – no mutex, no condvar, no futex.
//   • N must be a power of two (enforced by static_assert) so the modulo
//     operation becomes a bitwise AND.
//
// Usage:
//   Core 1 (network reader) calls TryPush().
//   Core 2 (matching engine) calls TryPop().
//   Zero contention on the hot path.
// ─────────────────────────────────────────────────────────────────────────────
#include <atomic>
#include <optional>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace chronos {

template<typename T, std::size_t N>
class SPSCRing {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");

    static constexpr std::size_t kMask = N - 1;

public:
    SPSCRing() = default;

    // ── Producer API (call from one thread only) ─────────────────────────
    [[nodiscard]] bool TryPush(const T& item) noexcept {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        const std::size_t next_h = (h + 1) & kMask;
        if (__builtin_expect(next_h == tail_.load(std::memory_order_acquire), 0))
            return false; // full
        buf_[h] = item;
        head_.store(next_h, std::memory_order_release);
        return true;
    }

    // ── Consumer API (call from one thread only) ─────────────────────────
    [[nodiscard]] bool TryPop(T& out) noexcept {
        const std::size_t t = tail_.load(std::memory_order_relaxed);

        if (t == head_.load(std::memory_order_acquire)) {
            return false;
        }

        out = buf_[t & kMask];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool Empty() const noexcept {
        return tail_.load(std::memory_order_acquire) ==
               head_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t Capacity() const noexcept { return N - 1; }

private:
    // Cache-line pad between head and tail to avoid false sharing.
    static constexpr std::size_t kPad = 64 / sizeof(std::size_t) - 1;

    alignas(64) std::atomic<std::size_t> head_ {0};
    std::size_t _pad0[kPad] {};

    alignas(64) std::atomic<std::size_t> tail_ {0};
    std::size_t _pad1[kPad] {};

    T buf_[N] {};
};

} // namespace chronos
