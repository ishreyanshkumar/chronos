#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// Chronos :: PriceLevel
//
// A doubly-linked intrusive FIFO queue of orders at one price point.
// Cancellation is O(1) because we can unlink any node directly without
// searching the list (the caller holds the Order* from the id→order map).
// ─────────────────────────────────────────────────────────────────────────────
#include "order.hpp"
#include <cstddef>

namespace chronos {

class PriceLevel {
public:
    PriceLevel() = default;

    // Push to tail (price-time priority: earlier orders execute first).
    void PushBack(Order* o) noexcept {
        o->prev = tail_;
        o->next = nullptr;
        if (tail_) tail_->next = o;
        else        head_      = o;
        tail_     = o;
        total_qty_ += o->Remaining();
        ++count_;
    }

    // Remove an arbitrary node – O(1) because pointers are intrusive.
    void Unlink(Order* o) noexcept {
        if (o->prev) o->prev->next = o->next;
        else         head_         = o->next;
        if (o->next) o->next->prev = o->prev;
        else         tail_         = o->prev;
        o->prev = o->next = nullptr;
        total_qty_ = (total_qty_ >= o->Remaining()) ? total_qty_ - o->Remaining() : 0;
        --count_;
    }

    void DeductQty(Quantity q) noexcept {
        total_qty_ = (total_qty_ >= q) ? total_qty_ - q : 0;
    }

    [[nodiscard]] Order*    Head()     const noexcept { return head_; }
    [[nodiscard]] bool      Empty()    const noexcept { return head_ == nullptr; }
    [[nodiscard]] Quantity  TotalQty() const noexcept { return total_qty_; }
    [[nodiscard]] std::size_t Count()  const noexcept { return count_; }

private:
    Order*    head_      {nullptr};
    Order*    tail_      {nullptr};
    Quantity  total_qty_ {0};
    std::size_t count_  {0};
};

} // namespace chronos
