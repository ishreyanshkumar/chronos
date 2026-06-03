#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// Chronos :: cpu_utils
//
// Utilities for core pinning, RDTSC-based nanosecond timing, and cache
// prefetching hints.  These are the low-level building blocks that separate
// a college project from production HFT infrastructure.
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdint>
#include <ctime>
#include <pthread.h>
#include <sched.h>

namespace chronos {

// ── Core pinning ─────────────────────────────────────────────────────────────
// Pin the calling thread to a specific logical CPU core.
// Returns 0 on success, errno otherwise.
inline int PinToCore(int core_id) noexcept {
#ifdef _WIN32
    (void)core_id;
    return 0;
#else
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(core_id, &cs);
    return ::pthread_setaffinity_np(::pthread_self(), sizeof(cs), &cs);
#endif
}

// ── High-resolution clock ─────────────────────────────────────────────────────
// clock_gettime(CLOCK_MONOTONIC_RAW) gives ~20 ns resolution without the
// vDSO overhead of gettimeofday and is immune to NTP adjustments.
inline uint64_t NowNanos() noexcept {
    struct timespec ts;
#ifdef _WIN32
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    ::clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#endif
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

// ── RDTSC timestamp counter ───────────────────────────────────────────────────
// For intra-core latency measurements where both start/end run on the same
// physical core.  Cheaper than clock_gettime (~5 cycles vs ~20 cycles).
inline uint64_t RDTSC() noexcept {
#if defined(__x86_64__)
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
#else
    // Fallback to CLOCK_MONOTONIC_RAW on non-x86
    return NowNanos();
#endif
}

// ── Prefetch hint ─────────────────────────────────────────────────────────────
template<typename T>
inline void Prefetch(const T* ptr) noexcept {
    __builtin_prefetch(ptr, 0, 1); // read, L2 locality
}

// ── Compiler fence (prevent reordering across measurement boundary) ───────────
inline void CompilerFence() noexcept {
    asm volatile ("" ::: "memory");
}

} // namespace chronos
