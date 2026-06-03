#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// Chronos :: Order
//
// The intrusive doubly-linked list is the key microarchitectural insight:
// prev/next pointers live *inside* the Order object.  When the CPU pulls an
// Order into L1 cache to read its price/qty, the list-traversal pointers come
// along for free – no secondary cache miss to chase an external node.
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdint>
#include <cstring>

namespace chronos {

// ── Side ─────────────────────────────────────────────────────────────────────
enum class Side : uint8_t { Buy = 0, Sell = 1 };

// ── OrderType ────────────────────────────────────────────────────────────────
enum class OrderType : uint8_t { Limit = 0, Market = 1, Cancel = 2 };

// ── Price representation ─────────────────────────────────────────────────────
// Prices are stored as integer cents (×10000) to avoid floating-point in the
// hot path.  $1.2345 → 12345.
using Price    = int64_t;
using Quantity = uint32_t;
using OrderId  = uint64_t;
using Nanos    = uint64_t;  // nanoseconds since epoch

static constexpr Price kMarketPrice = 0;  // sentinel for market orders

// ── Order ────────────────────────────────────────────────────────────────────
// Size is intentionally kept ≤ 64 bytes (one cache line).
struct alignas(64) Order {
    // ── Identity ──────────────────────────────────────────────────────────
    OrderId   id        {0};
    Price     price     {0};
    Quantity  quantity  {0};
    Quantity  filled    {0};
    Side      side      {Side::Buy};
    OrderType type      {OrderType::Limit};
    uint8_t   _pad[2]   {};

    // ── Intrusive list pointers ───────────────────────────────────────────
    // Embedded directly so L1 prefetch brings them in with the order data.
    Order*  prev {nullptr};
    Order*  next {nullptr};

    // ── Timestamp ─────────────────────────────────────────────────────────
    Nanos   ts_ns {0};

    // ── Helpers ───────────────────────────────────────────────────────────
    [[nodiscard]] Quantity Remaining() const noexcept { return quantity - filled; }
    [[nodiscard]] bool     IsFilled()  const noexcept { return filled >= quantity; }
};

static_assert(sizeof(Order) <= 64, "Order must fit in one cache line");

// ── Trade report (written to SPSC log) ───────────────────────────────────────
struct Trade {
    OrderId  buy_id;
    OrderId  sell_id;
    Price    price;
    Quantity quantity;
    Nanos    ts_ns;
};

} // namespace chronos
