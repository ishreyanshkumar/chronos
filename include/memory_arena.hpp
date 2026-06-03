#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// Chronos :: MemoryArena
//
// Optimized slab allocator using a pre-faulted std::vector.
// By utilizing T::next for the free-list, we avoid reinterpret_casts 
// and improve memory safety and speed.
// ─────────────────────────────────────────────────────────────────────────────
#include <cstddef>
#include <cstdint>
#include <cassert>
#include <vector>

namespace chronos {

template<typename T, std::size_t Capacity>
class MemoryArena {
public:
    static_assert(Capacity > 0, "Capacity must be positive");

    MemoryArena() {
        pool_.resize(Capacity);
        // Pre-fault and link free list
        for (std::size_t i = 0; i + 1 < Capacity; ++i) {
            pool_[i].next = &pool_[i + 1];
        }
        pool_.back().next = nullptr;
        free_head_ = &pool_[0];
    }

    // Non-copyable, non-movable.
    MemoryArena(const MemoryArena&)            = delete;
    MemoryArena& operator=(const MemoryArena&) = delete;

    [[nodiscard]] T* Alloc() noexcept {
        if (__builtin_expect(free_head_ == nullptr, 0)) return nullptr;
        T* slot     = free_head_;
        free_head_  = slot->next;
        slot->prev  = nullptr;
        slot->next  = nullptr;
        ++live_count_;
        return slot;
    }

    void Free(T* ptr) noexcept {
        assert(ptr != nullptr);
        ptr->next = free_head_;
        ptr->prev = nullptr;
        free_head_ = ptr;
        --live_count_;
    }

    [[nodiscard]] std::size_t LiveCount()  const noexcept { return live_count_; }
    [[nodiscard]] std::size_t FreeSlots()  const noexcept { return Capacity - live_count_; }
    [[nodiscard]] bool        Full()       const noexcept { return live_count_ == Capacity; }

private:
    std::vector<T> pool_;
    T*             free_head_  {nullptr};
    std::size_t    live_count_ {0};
};

} // namespace chronos
