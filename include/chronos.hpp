#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <functional>
#include <atomic>
#include <cassert>
#include <string>
#include <pthread.h>
#include <sched.h>
#include <fstream>
#include <thread>


namespace chronos {

enum class Side : uint8_t { Buy = 0, Sell = 1 };
enum class OrderType : uint8_t { Limit = 0, Market = 1, Cancel = 2 };

using Price = int64_t;
using Quantity = uint32_t;
using OrderId = uint64_t;
using Nanos = uint64_t;

// ── Core Data Structures ───────────────────────────────────────────────────────

// kMarketPrice is used to denote an aggressive market order that sweeps the book
static constexpr Price kMarketPrice = 0;

// The Order struct is perfectly aligned to exactly 64 bytes. 
// This ensures it fits perfectly into a single L1 CPU cache line, preventing 
// the CPU from having to fetch multiple cache lines to read a single order.
struct alignas(64) Order {
    OrderId id;
    Price price;
    Quantity quantity;
    Quantity filled;
    Side side;
    OrderType type;
    uint8_t _pad[2]; // Padding to align perfectly to 64 bytes

    // Intrusive linked list pointers. 
    // By keeping prev/next inside the order data, we eliminate cache misses 
    // that would occur if we used a separate Node struct (like std::list does).
    Order* prev;
    Order* next;
    Nanos ts_ns; // Timestamp of order arrival for price-time priority

    Quantity Remaining() const { return quantity - filled; }
    bool IsFilled() const { return filled >= quantity; }
};

struct Trade {
    OrderId buy_id;
    OrderId sell_id;
    Price price;
    Quantity quantity;
    Nanos ts_ns;
};

// ── Pre-faulted Memory Arena ─────────────────────────────────────────────────

// A zero-allocation memory arena. Instead of calling malloc() or new on the hot path,
// we pre-allocate all orders upfront in a giant array (pool_) and link them together 
// into a free-list. Allocating an order takes O(1) time (a single pointer chase).
template<typename T, std::size_t Capacity>
class MemoryArena {
public:
    MemoryArena() {
        pool_.resize(Capacity);
        for (std::size_t i = 0; i + 1 < Capacity; ++i) {
            pool_[i].next = &pool_[i + 1];
        }
        pool_.back().next = nullptr;
        free_head_ = &pool_[0];
    }

    // Grab the first available object from the free-list. No OS involvement.
    T* Alloc() {
        if (!free_head_) return nullptr;
        T* slot = free_head_;
        free_head_ = slot->next;
        slot->prev = nullptr;
        slot->next = nullptr;
        ++live_count_;
        return slot;
    }

    // Return an object to the free-list instantly.
    void Free(T* ptr) {
        ptr->next = free_head_;
        ptr->prev = nullptr;
        free_head_ = ptr;
        --live_count_;
    }

    std::size_t LiveCount() const { return live_count_; }

private:
    std::vector<T> pool_;
    T* free_head_ = nullptr;
    std::size_t live_count_ = 0;
};

// ── Price Level Management ───────────────────────────────────────────────────

// Represents a single price point in the order book. Contains a doubly-linked 
// FIFO queue of resting orders to enforce Price-Time Priority.
class PriceLevel {
public:
    // Append to the back of the queue (O(1) complexity)
    void PushBack(Order* o) {
        o->prev = tail_;
        o->next = nullptr;
        if (tail_) tail_->next = o;
        else head_ = o;
        tail_ = o;
        total_qty_ += o->Remaining();
    }

    // Unlink an order instantly. Because the order has `prev` and `next` pointers,
    // we can remove it in O(1) time without having to traverse the list.
    void Unlink(Order* o) {
        if (o->prev) o->prev->next = o->next;
        else head_ = o->next;
        if (o->next) o->next->prev = o->prev;
        else tail_ = o->prev;
        o->prev = o->next = nullptr;
        total_qty_ -= o->Remaining();
    }

    void DeductQty(Quantity q) {
        total_qty_ -= q;
    }

    Order* Head() const { return head_; }
    bool Empty() const { return head_ == nullptr; }

private:
    Order* head_ = nullptr;
    Order* tail_ = nullptr;
    Quantity total_qty_ = 0;
};

using TradeCallback = std::function<void(const Trade&)>;

// ── Matching Engine Core ─────────────────────────────────────────────────────

// The main OrderBook. It maintains vectors for bids and asks, providing O(1) 
// random access to any price level to prevent Red-Black tree (std::map) overhead.
class OrderBook {
public:
    explicit OrderBook(TradeCallback cb = nullptr);

    int AddOrder(OrderId id, Side side, OrderType type, Price price, Quantity qty, Nanos ts);
    bool CancelOrder(OrderId id);

    Price BestBid() const;
    Price BestAsk() const;

    uint64_t TotalFills() const { return fills_; }
    std::size_t OrderCount() const { return active_orders_; }

private:
    int Match(Order* incoming);
    void Fill(Order* passive, Order* aggressive, Price fill_price, Quantity fill_qty);

    void RecalcBestBid() const;
    void RecalcBestAsk() const;

    // Use dense vectors (arrays) rather than maps for instant O(1) price lookups
    static constexpr std::size_t kMaxPriceTicks = 2000000;
    std::vector<PriceLevel> bids_;
    std::vector<PriceLevel> asks_;
    
    // Direct pointer mapping from OrderId to memory arena object for O(1) cancels
    std::vector<Order*> id_map_;

    MemoryArena<Order, 1 << 22> arena_;
    TradeCallback on_trade_;

    mutable Price best_bid_ = -1;
    mutable Price best_ask_ = -1;

    uint64_t fills_ = 0;
    std::size_t active_orders_ = 0;
};

// ── Thread Isolation Queue ───────────────────────────────────────────────────

// A wait-free, lock-free Single-Producer Single-Consumer ring buffer.
// Used to pass data between the Network core and the Matching Engine core 
// without mutexes or context switches.
template<typename T, std::size_t N>
class SPSCRing {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");
    static constexpr std::size_t kMask = N - 1;
    static constexpr std::size_t kPad = 64 / sizeof(std::size_t) - 1;

public:
    bool TryPush(const T& item) {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        const std::size_t next_h = (h + 1) & kMask;
        if (next_h == tail_.load(std::memory_order_acquire))
            return false;
        buf_[h] = item;
        head_.store(next_h, std::memory_order_release);
        return true;
    }

    bool TryPop(T& out) {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire))
            return false;
        out = buf_[t];
        tail_.store((t + 1) & kMask, std::memory_order_release);
        return true;
    }

    bool Empty() const {
        return tail_.load(std::memory_order_acquire) ==
               head_.load(std::memory_order_acquire);
    }

private:
    // Pad the head_ and tail_ atomics to 64 bytes so they sit on separate 
    // cache lines. This completely eliminates False Sharing, allowing 
    // both cores to spin on their respective variables without cache invalidation.
    alignas(64) std::atomic<std::size_t> head_ {0};
    std::size_t _pad0[kPad] {};
    alignas(64) std::atomic<std::size_t> tail_ {0};
    std::size_t _pad1[kPad] {};
    T buf_[N] {};
};

struct MsgAdd {
    OrderId order_ref;
    Side side;
    Quantity shares;
    char stock[9];
    Price price;
    Nanos ts_ns;
};

struct MsgExecuted {
    OrderId order_ref;
    Quantity shares;
    Nanos ts_ns;
};

struct MsgCancelled {
    OrderId order_ref;
    Quantity shares;
    Nanos ts_ns;
};

struct MsgDeleted {
    OrderId order_ref;
    Nanos ts_ns;
};

struct MsgReplaced {
    OrderId old_ref;
    OrderId new_ref;
    Quantity shares;
    Price price;
    Nanos ts_ns;
};

struct ITCHCallbacks {
    std::function<void(const MsgAdd&)> on_add;
    std::function<void(const MsgExecuted&)> on_executed;
    std::function<void(const MsgCancelled&)> on_cancelled;
    std::function<void(const MsgDeleted&)> on_deleted;
    std::function<void(const MsgReplaced&)> on_replaced;
};

// ── ITCH Network Protocol Parsing ────────────────────────────────────────────

// A zero-copy ITCH 5.0 protocol parser. 
class ITCHParser {
public:
    // Parses a file via mmap, completely bypassing kernel space copies
    std::size_t ParseFile(const std::string& path, const ITCHCallbacks& cbs);
    static std::size_t ParseMessage(const uint8_t* buf, std::size_t len, const ITCHCallbacks& cbs);
};



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
