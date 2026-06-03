#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// Chronos :: OrderBook
//
// Core Limit Order Book (LOB) engine.
//
// Data structures:
//   • Bids: Dense flat array (std::vector<PriceLevel>) for O(1) lookup.
//   • Asks: Dense flat array (std::vector<PriceLevel>) for O(1) lookup.
//   • id_map_: Dense flat array (std::vector<Order*>) for O(1) lookup by ID.
//
// Memory: all Order objects live in a pre-faulted MemoryArena – the STL maps
// only hold PriceLevel values (small structs, not heap-allocated per order).
//
// Hot-path latency target: < 80 ns median for Add/Cancel on a warm L1 cache.
// ─────────────────────────────────────────────────────────────────────────────
#include "order.hpp"
#include "price_level.hpp"
#include "memory_arena.hpp"
#include "spsc_ring.hpp"

#include <map>
#include <unordered_map>
#include <functional>
#include <optional>

namespace chronos {

// ── Arena sizing ─────────────────────────────────────────────────────────────
static constexpr std::size_t kArenaCapacity = 1 << 22; // 4 M orders in flight

// ── Trade callback ────────────────────────────────────────────────────────────
// Called synchronously inside Match() for every fill.
using TradeCallback = std::function<void(const Trade&)>;

// ── OrderBook ────────────────────────────────────────────────────────────────
class OrderBook {
public:
    explicit OrderBook(TradeCallback cb = nullptr);

    // Returns the number of fills generated.
    int AddOrder(OrderId id, Side side, OrderType type,
                 Price price, Quantity qty, Nanos ts);

    // Returns true if the order was found and cancelled.
    bool CancelOrder(OrderId id);

    // ── Accessors (read-only, const) ──────────────────────────────────────
    [[nodiscard]] std::optional<Price> BestBid() const noexcept;
    [[nodiscard]] std::optional<Price> BestAsk() const noexcept;
    [[nodiscard]] std::size_t          OrderCount()  const noexcept;

    // ── Statistics ────────────────────────────────────────────────────────
    [[nodiscard]] uint64_t TotalAdds()    const noexcept { return stats_.adds; }
    [[nodiscard]] uint64_t TotalFills()   const noexcept { return stats_.fills; }
    [[nodiscard]] uint64_t TotalCancels() const noexcept { return stats_.cancels; }

private:
    // Match an incoming order against the opposite side.
    int Match(Order* incoming);

    // Execute a single fill between two orders at fill_price / fill_qty.
    void Fill(Order* passive, Order* aggressive, Price fill_price, Quantity fill_qty);

    // Remove a price level from the book if it becomes empty.
    // (No longer needed since we use dense vectors, but kept for logic structure)
    void RecalcBestBid() const noexcept;
    void RecalcBestAsk() const noexcept;

    // Using vectors sized to kMaxPriceTicks for O(1) lookup.
    static constexpr std::size_t kMaxPriceTicks = 2'000'000;
    std::vector<PriceLevel> bids_;
    std::vector<PriceLevel> asks_;
    
    // Using a dense array for OrderId mapping for O(1) lookup.
    // Assuming OrderId starts at 1 and stays within bounds.
    std::vector<Order*> id_map_;

    MemoryArena<Order, kArenaCapacity> arena_;

    TradeCallback on_trade_;

    mutable std::optional<Price> best_bid_ = std::nullopt;
    mutable std::optional<Price> best_ask_ = std::nullopt;

    std::size_t active_orders_ = 0;

    struct Stats {
        uint64_t adds    {0};
        uint64_t fills   {0};
        uint64_t cancels {0};
    } stats_;
};

} // namespace chronos
