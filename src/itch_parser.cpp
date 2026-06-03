// ─────────────────────────────────────────────────────────────────────────────
// Chronos :: ITCHParser implementation
// ─────────────────────────────────────────────────────────────────────────────
#include "itch_parser.hpp"

#include <fcntl.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>

namespace chronos {

// ── Byte-order helpers (ITCH is big-endian) ───────────────────────────────────
uint32_t ITCHParser::ReadU32BE(const uint8_t* p) noexcept {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) <<  8) |  uint32_t(p[3]);
}

uint64_t ITCHParser::ReadU64BE(const uint8_t* p) noexcept {
    return (uint64_t(p[0]) << 56) | (uint64_t(p[1]) << 48) |
           (uint64_t(p[2]) << 40) | (uint64_t(p[3]) << 32) |
           (uint64_t(p[4]) << 24) | (uint64_t(p[5]) << 16) |
           (uint64_t(p[6]) <<  8) |  uint64_t(p[7]);
}

// ITCH timestamps are 6-byte big-endian nanoseconds since midnight.
Nanos ITCHParser::ReadNanos(const uint8_t* p) noexcept {
    return (uint64_t(p[0]) << 40) | (uint64_t(p[1]) << 32) |
           (uint64_t(p[2]) << 24) | (uint64_t(p[3]) << 16) |
           (uint64_t(p[4]) <<  8) |  uint64_t(p[5]);
}

// ITCH prices: 4-byte unsigned integer representing price × 10000.
Price ITCHParser::ReadPrice4(const uint8_t* p) noexcept {
    return static_cast<Price>(ReadU32BE(p));
}

// ── ParseMessage ──────────────────────────────────────────────────────────────
// ITCH 5.0 messages are length-prefixed with a 2-byte big-endian length field.
// This function parses ONE message body (the caller has already consumed the
// 2-byte length prefix and verified there are enough bytes).
std::size_t ITCHParser::ParseMessage(const uint8_t* buf, std::size_t len,
                                      const ITCHCallbacks& cbs) {
    if (len < 3) return 0;

    const uint8_t msg_type = buf[0];

    switch (msg_type) {
    // ── Add Order (no MPID) ────────────────────────────────────────────────
    case 'A': {
        // Offset layout (after msg_type byte):
        //   1..2  : Stock Locate (2)
        //   3..4  : Tracking Number (2)
        //   5..10 : Timestamp (6)
        //   11..18: Order Reference (8)
        //   19    : Buy/Sell Indicator (1)
        //   20..23: Shares (4)
        //   24..31: Stock (8)
        //   32..35: Price (4)
        if (len < 36) return 0;
        MsgAdd m{};
        m.ts_ns     = ReadNanos(buf + 5);
        m.order_ref = ReadU64BE(buf + 11);
        m.side      = (buf[19] == 'B') ? Side::Buy : Side::Sell;
        m.shares    = ReadU32BE(buf + 20);
        std::memcpy(m.stock, buf + 24, 8); m.stock[8] = '\0';
        m.price     = ReadPrice4(buf + 32);
        if (cbs.on_add) cbs.on_add(m);
        return 36;
    }

    // ── Add Order with MPID ────────────────────────────────────────────────
    case 'F': {
        if (len < 40) return 0;
        MsgAdd m{};
        m.ts_ns     = ReadNanos(buf + 5);
        m.order_ref = ReadU64BE(buf + 11);
        m.side      = (buf[19] == 'B') ? Side::Buy : Side::Sell;
        m.shares    = ReadU32BE(buf + 20);
        std::memcpy(m.stock, buf + 24, 8); m.stock[8] = '\0';
        m.price     = ReadPrice4(buf + 32);
        // buf[36..39] = MPID (ignored)
        if (cbs.on_add) cbs.on_add(m);
        return 40;
    }

    // ── Order Executed ─────────────────────────────────────────────────────
    case 'E': {
        if (len < 31) return 0;
        MsgExecuted m{};
        m.ts_ns     = ReadNanos(buf + 5);
        m.order_ref = ReadU64BE(buf + 11);
        m.shares    = ReadU32BE(buf + 19);
        if (cbs.on_executed) cbs.on_executed(m);
        return 31;
    }

    // ── Order Cancelled (partial) ──────────────────────────────────────────
    case 'X': {
        if (len < 23) return 0;
        MsgCancelled m{};
        m.ts_ns     = ReadNanos(buf + 5);
        m.order_ref = ReadU64BE(buf + 11);
        m.shares    = ReadU32BE(buf + 19);
        if (cbs.on_cancelled) cbs.on_cancelled(m);
        return 23;
    }

    // ── Order Deleted ──────────────────────────────────────────────────────
    case 'D': {
        if (len < 19) return 0;
        MsgDeleted m{};
        m.ts_ns     = ReadNanos(buf + 5);
        m.order_ref = ReadU64BE(buf + 11);
        if (cbs.on_deleted) cbs.on_deleted(m);
        return 19;
    }

    // ── Order Replaced ────────────────────────────────────────────────────
    case 'U': {
        if (len < 35) return 0;
        MsgReplaced m{};
        m.ts_ns     = ReadNanos(buf + 5);
        m.old_ref   = ReadU64BE(buf + 11);
        m.new_ref   = ReadU64BE(buf + 19);
        m.shares    = ReadU32BE(buf + 27);
        m.price     = ReadPrice4(buf + 31);
        if (cbs.on_replaced) cbs.on_replaced(m);
        return 35;
    }

    default:
        // Unknown / unsupported message type – skip by returning 0.
        // The caller will advance past the declared message length.
        return 0;
    }
}

// ── ParseFile ────────────────────────────────────────────────────────────────
std::size_t ITCHParser::ParseFile(const std::string& path,
                                   const ITCHCallbacks& cbs) {
#ifdef _WIN32
    HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open ITCH file: " + path);

    LARGE_INTEGER li;
    if (!GetFileSizeEx(hFile, &li)) { CloseHandle(hFile); throw std::runtime_error("Cannot get size"); }
    const std::size_t file_size = static_cast<std::size_t>(li.QuadPart);
    if (file_size == 0) { CloseHandle(hFile); return 0; }

    HANDLE hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!hMap) { CloseHandle(hFile); throw std::runtime_error("CreateFileMapping failed"); }

    void* raw = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!raw) { CloseHandle(hMap); CloseHandle(hFile); throw std::runtime_error("MapViewOfFile failed"); }

    const auto* buf = reinterpret_cast<const uint8_t*>(raw);
    std::size_t pos = 0;
    std::size_t count = 0;

    while (pos + 2 <= file_size) {
        uint16_t msg_len = (uint16_t(buf[pos]) << 8) | buf[pos + 1];
        pos += 2;
        if (pos + msg_len > file_size) break;
        ParseMessage(buf + pos, msg_len, cbs);
        pos += msg_len;
        ++count;
    }

    UnmapViewOfFile(raw);
    CloseHandle(hMap);
    CloseHandle(hFile);
    return count;
#else
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) throw std::runtime_error("Cannot open ITCH file: " + path);

    struct stat st{};
    ::fstat(fd, &st);
    const std::size_t file_size = static_cast<std::size_t>(st.st_size);

    if (file_size == 0) { ::close(fd); return 0; }

    void* raw = ::mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (raw == MAP_FAILED) throw std::runtime_error("mmap failed for: " + path);

    // Tell the kernel we will read sequentially – triggers aggressive readahead.
    ::madvise(raw, file_size, MADV_SEQUENTIAL);

    const auto* buf = reinterpret_cast<const uint8_t*>(raw);
    std::size_t pos = 0;
    std::size_t count = 0;

    while (pos + 2 <= file_size) {
        // ITCH 5.0: each message is prefixed by a 2-byte BE message length.
        uint16_t msg_len = (uint16_t(buf[pos]) << 8) | buf[pos + 1];
        pos += 2;

        if (pos + msg_len > file_size) break; // truncated

        std::size_t consumed = ParseMessage(buf + pos, msg_len, cbs);
        (void)consumed; // we advance by msg_len regardless
        pos += msg_len;
        ++count;
    }

    ::munmap(raw, file_size);
    return count;
#endif
}

} // namespace chronos
