// ─────────────────────────────────────────────────────────────────────────────
// Chronos :: OrderBook implementation
// ─────────────────────────────────────────────────────────────────────────────
#include "order_book.hpp"
#include "cpu_utils.hpp"
#include <cassert>

namespace chronos {

OrderBook::OrderBook(TradeCallback cb)
    : bids_(kMaxPriceTicks), asks_(kMaxPriceTicks), id_map_(4'000'000, nullptr), on_trade_(std::move(cb)) {}

void OrderBook::RecalcBestBid() const noexcept {
    if (!best_bid_) return;
    for (Price p = *best_bid_; p > 0; --p) {
        if (!bids_[p].Empty()) {
            best_bid_ = p;
            return;
        }
    }
    best_bid_ = std::nullopt;
}

void OrderBook::RecalcBestAsk() const noexcept {
    if (!best_ask_) return;
    for (Price p = *best_ask_; p < kMaxPriceTicks; ++p) {
        if (!asks_[p].Empty()) {
            best_ask_ = p;
            return;
        }
    }
    best_ask_ = std::nullopt;
}

int OrderBook::AddOrder(OrderId id, Side side, OrderType type,
                         Price price, Quantity qty, Nanos ts) {
    assert(qty > 0);
    assert(price < kMaxPriceTicks);

    Order* o = arena_.Alloc();
    if (__builtin_expect(o == nullptr, 0)) return -1; // arena exhausted

    // Placement-new initialises the object inside the arena slot.
    new (o) Order{
        .id       = id,
        .price    = price,
        .quantity = qty,
        .filled   = 0,
        .side     = side,
        .type     = type,
        .prev     = nullptr,
        .next     = nullptr,
        .ts_ns    = ts,
    };

    ++stats_.adds;
    ++active_orders_;

    if (type == OrderType::Cancel) {
        // Treat as an immediate cancel-by-id (the generator uses this path).
        bool ok = CancelOrder(id);
        arena_.Free(o);
        --active_orders_;
        return ok ? 0 : -1;
    }

    if (id >= id_map_.size()) {
        id_map_.resize(std::max(id_map_.size() * 2, static_cast<size_t>(id + 1000)), nullptr);
    }
    id_map_[id] = o;

    int fills = Match(o);

    // If order is not fully filled and it is a limit order, rest it on the book.
    if (!o->IsFilled() && type == OrderType::Limit) {
        if (side == Side::Buy) {
            bids_[price].PushBack(o);
            if (!best_bid_ || price > *best_bid_) best_bid_ = price;
        } else {
            asks_[price].PushBack(o);
            if (!best_ask_ || price < *best_ask_) best_ask_ = price;
        }
    } else if (o->IsFilled() || type == OrderType::Market) {
        // Fully matched or market order with no resting qty – free the slot.
        id_map_[id] = nullptr;
        arena_.Free(o);
        --active_orders_;
    }

    return fills;
}

bool OrderBook::CancelOrder(OrderId id) {
    if (id >= id_map_.size()) return false;
    Order* o = id_map_[id];
    if (!o) return false;

    if (o->side == Side::Buy) {
        bids_[o->price].Unlink(o);
        if (best_bid_ && o->price == *best_bid_ && bids_[o->price].Empty()) {
            RecalcBestBid();
        }
    } else {
        asks_[o->price].Unlink(o);
        if (best_ask_ && o->price == *best_ask_ && asks_[o->price].Empty()) {
            RecalcBestAsk();
        }
    }

    id_map_[id] = nullptr;
    arena_.Free(o);
    ++stats_.cancels;
    --active_orders_;
    return true;
}

std::optional<Price> OrderBook::BestBid() const noexcept { 
    RecalcBestBid();
    return best_bid_; 
}

std::optional<Price> OrderBook::BestAsk() const noexcept { 
    RecalcBestAsk();
    return best_ask_; 
}

std::size_t OrderBook::OrderCount() const noexcept {
    return active_orders_;
}

int OrderBook::Match(Order* incoming) {
    int fills = 0;

    if (incoming->side == Side::Buy) {
        // Buy crosses with the best (lowest) ask.
        while (!incoming->IsFilled() && best_ask_) {
            Price ask_price = *best_ask_;
            // Market orders match any price; limit orders only cross the spread.
            if (incoming->type == OrderType::Limit && incoming->price < ask_price)
                break;

            PriceLevel& level = asks_[ask_price];
            Order* cur = level.Head();
            while (cur && !incoming->IsFilled()) {
                Order* passive = cur;
                Quantity fill_qty = std::min(incoming->Remaining(), passive->Remaining());
                Fill(passive, incoming, ask_price, fill_qty);
                level.DeductQty(fill_qty);
                ++fills;

                if (passive->IsFilled()) {
                    cur = cur->next;
                    level.Unlink(passive);
                    id_map_[passive->id] = nullptr;
                    arena_.Free(passive);
                    --active_orders_;
                } else {
                    break;
                }
            }
            if (level.Empty()) {
                if (ask_price < kMaxPriceTicks - 1) best_ask_ = ask_price + 1;
                else best_ask_ = std::nullopt;
            }
        }
    } else {
        // Sell crosses with the best (highest) bid.
        while (!incoming->IsFilled() && best_bid_) {
            Price bid_price = *best_bid_;
            if (incoming->type == OrderType::Limit && incoming->price > bid_price)
                break;

            PriceLevel& level = bids_[bid_price];
            Order* cur = level.Head();
            while (cur && !incoming->IsFilled()) {
                Order* passive = cur;
                Quantity fill_qty = std::min(incoming->Remaining(), passive->Remaining());
                Fill(passive, incoming, bid_price, fill_qty);
                level.DeductQty(fill_qty);
                ++fills;

                if (passive->IsFilled()) {
                    cur = cur->next;
                    level.Unlink(passive);
                    id_map_[passive->id] = nullptr;
                    arena_.Free(passive);
                    --active_orders_;
                } else {
                    break;
                }
            }
            if (level.Empty()) {
                if (bid_price > 0) best_bid_ = bid_price - 1;
                else best_bid_ = std::nullopt;
            }
        }
    }

    return fills;
}

void OrderBook::Fill(Order* passive, Order* aggressive,
                     Price fill_price, Quantity fill_qty) {
    passive->filled    += fill_qty;
    aggressive->filled += fill_qty;

    Trade t{};
    if (aggressive->side == Side::Buy) {
        t.buy_id  = aggressive->id;
        t.sell_id = passive->id;
    } else {
        t.buy_id  = passive->id;
        t.sell_id = aggressive->id;
    }
    t.price    = fill_price;
    t.quantity = fill_qty;
    t.ts_ns    = aggressive->ts_ns; // Re-use the incoming order's timestamp instead of a costly clock_gettime syscall

    ++stats_.fills;

    if (on_trade_) on_trade_(t);
}

} // namespace chronos
