#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// Chronos :: ITCHParser
//
// Zero-copy NASDAQ TotalView-ITCH 5.0 parser.
//
// The file is mapped into the process address space with mmap() so the kernel
// never copies packet bytes into userspace buffers – the CPU reads straight
// from the page cache.  madvise(MADV_SEQUENTIAL) tells the kernel to
// aggressively read-ahead pages, keeping the parser fed from L3/RAM.
//
// Supported message types (subset sufficient for order book reconstruction):
//   'A' – Add Order (no MPID)
//   'F' – Add Order (with MPID, treated identically)
//   'E' – Order Executed
//   'X' – Order Cancelled (partial)
//   'D' – Order Deleted
//   'U' – Order Replaced
// ─────────────────────────────────────────────────────────────────────────────
#include "order.hpp"
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>

namespace chronos {

// ── Parsed message variants ───────────────────────────────────────────────────

struct MsgAdd {
    OrderId   order_ref;
    Side      side;
    Quantity  shares;
    char      stock[9];
    Price     price;   // already converted to int64 cents×10000
    Nanos     ts_ns;
};

struct MsgExecuted {
    OrderId   order_ref;
    Quantity  shares;
    Nanos     ts_ns;
};

struct MsgCancelled {
    OrderId   order_ref;
    Quantity  shares;
    Nanos     ts_ns;
};

struct MsgDeleted {
    OrderId   order_ref;
    Nanos     ts_ns;
};

struct MsgReplaced {
    OrderId   old_ref;
    OrderId   new_ref;
    Quantity  shares;
    Price     price;
    Nanos     ts_ns;
};

// ── Callbacks ────────────────────────────────────────────────────────────────
struct ITCHCallbacks {
    std::function<void(const MsgAdd&)>       on_add;
    std::function<void(const MsgExecuted&)>  on_executed;
    std::function<void(const MsgCancelled&)> on_cancelled;
    std::function<void(const MsgDeleted&)>   on_deleted;
    std::function<void(const MsgReplaced&)>  on_replaced;
};

// ── Parser ────────────────────────────────────────────────────────────────────
class ITCHParser {
public:
    ITCHParser() = default;

    // Parse a raw ITCH 5.0 binary file (or mmap'd region).
    // Returns the number of messages processed.
    std::size_t ParseFile(const std::string& path, const ITCHCallbacks& cbs);

    // Parse a single ITCH message from a byte buffer.
    // Returns bytes consumed (0 on unknown type).
    static std::size_t ParseMessage(const uint8_t* buf, std::size_t len,
                                    const ITCHCallbacks& cbs);

private:
    static Nanos     ReadNanos(const uint8_t* p) noexcept;
    static uint32_t  ReadU32BE(const uint8_t* p) noexcept;
    static uint64_t  ReadU64BE(const uint8_t* p) noexcept;
    static Price     ReadPrice4(const uint8_t* p) noexcept; // 4-byte BE ×10000
};

} // namespace chronos
