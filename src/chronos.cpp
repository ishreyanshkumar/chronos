#include "chronos.hpp"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>
#include <algorithm>

namespace chronos {

OrderBook::OrderBook(TradeCallback cb)
    : bids_(kMaxPriceTicks), asks_(kMaxPriceTicks), id_map_(4000000, nullptr), on_trade_(std::move(cb)) {}

void OrderBook::RecalcBestBid() const {
    if (best_bid_ == -1) return;
    for (Price p = best_bid_; p > 0; --p) {
        if (!bids_[p].Empty()) {
            best_bid_ = p;
            return;
        }
    }
    best_bid_ = -1;
}

void OrderBook::RecalcBestAsk() const {
    if (best_ask_ == -1) return;
    for (Price p = best_ask_; p < (Price)kMaxPriceTicks; ++p) {
        if (!asks_[p].Empty()) {
            best_ask_ = p;
            return;
        }
    }
    best_ask_ = -1;
}

int OrderBook::AddOrder(OrderId id, Side side, OrderType type, Price price, Quantity qty, Nanos ts) {
    Order* o = arena_.Alloc();
    if (!o) return -1;

    o->id = id;
    o->price = price;
    o->quantity = qty;
    o->filled = 0;
    o->side = side;
    o->type = type;
    o->prev = nullptr;
    o->next = nullptr;
    o->ts_ns = ts;

    if (type == OrderType::Cancel) {
        bool ok = CancelOrder(id);
        arena_.Free(o);
        return ok ? 0 : -1;
    }

    ++active_orders_;

    if (id >= id_map_.size()) {
        id_map_.resize(std::max(id_map_.size() * 2, static_cast<size_t>(id + 1000)), nullptr);
    }
    id_map_[id] = o;

    int fills = Match(o);

    if (!o->IsFilled() && type == OrderType::Limit) {
        if (side == Side::Buy) {
            bids_[price].PushBack(o);
            if (best_bid_ == -1 || price > best_bid_) best_bid_ = price;
        } else {
            asks_[price].PushBack(o);
            if (best_ask_ == -1 || price < best_ask_) best_ask_ = price;
        }
    } else if (o->IsFilled() || type == OrderType::Market) {
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
        if (best_bid_ != -1 && o->price == best_bid_ && bids_[o->price].Empty()) {
            RecalcBestBid();
        }
    } else {
        asks_[o->price].Unlink(o);
        if (best_ask_ != -1 && o->price == best_ask_ && asks_[o->price].Empty()) {
            RecalcBestAsk();
        }
    }

    id_map_[id] = nullptr;
    arena_.Free(o);
    --active_orders_;
    return true;
}

Price OrderBook::BestBid() const { 
    RecalcBestBid();
    return best_bid_; 
}

Price OrderBook::BestAsk() const { 
    RecalcBestAsk();
    return best_ask_; 
}

int OrderBook::Match(Order* incoming) {
    int fills = 0;

    if (incoming->side == Side::Buy) {
        while (!incoming->IsFilled() && best_ask_ != -1) {
            Price ask_price = best_ask_;
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
                if (ask_price < (Price)kMaxPriceTicks - 1) best_ask_ = ask_price + 1;
                else best_ask_ = -1;
            }
        }
    } else {
        while (!incoming->IsFilled() && best_bid_ != -1) {
            Price bid_price = best_bid_;
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
                } else {
                    break;
                }
            }
            if (level.Empty()) {
                if (bid_price > 0) best_bid_ = bid_price - 1;
                else best_bid_ = -1;
            }
        }
    }

    return fills;
}

void OrderBook::Fill(Order* passive, Order* aggressive, Price fill_price, Quantity fill_qty) {
    passive->filled += fill_qty;
    aggressive->filled += fill_qty;

    Trade t{};
    if (aggressive->side == Side::Buy) {
        t.buy_id = aggressive->id;
        t.sell_id = passive->id;
    } else {
        t.buy_id = passive->id;
        t.sell_id = aggressive->id;
    }
    t.price = fill_price;
    t.quantity = fill_qty;
    t.ts_ns = aggressive->ts_ns;

    ++fills_;

    if (on_trade_) on_trade_(t);
}

// ITCHParser

static uint32_t ReadU32BE(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

static uint64_t ReadU64BE(const uint8_t* p) {
    return (uint64_t(p[0]) << 56) | (uint64_t(p[1]) << 48) | (uint64_t(p[2]) << 40) | (uint64_t(p[3]) << 32) |
           (uint64_t(p[4]) << 24) | (uint64_t(p[5]) << 16) | (uint64_t(p[6]) << 8)  | uint64_t(p[7]);
}

static Nanos ReadNanos(const uint8_t* p) {
    return (uint64_t(p[0]) << 40) | (uint64_t(p[1]) << 32) | (uint64_t(p[2]) << 24) | (uint64_t(p[3]) << 16) |
           (uint64_t(p[4]) << 8)  | uint64_t(p[5]);
}

static Price ReadPrice4(const uint8_t* p) {
    return static_cast<Price>(ReadU32BE(p));
}

std::size_t ITCHParser::ParseMessage(const uint8_t* buf, std::size_t len, const ITCHCallbacks& cbs) {
    if (len < 3) return 0;
    const uint8_t msg_type = buf[0];

    switch (msg_type) {
    case 'A': {
        if (len < 36) return 0;
        MsgAdd m{};
        m.ts_ns = ReadNanos(buf + 5);
        m.order_ref = ReadU64BE(buf + 11);
        m.side = (buf[19] == 'B') ? Side::Buy : Side::Sell;
        m.shares = ReadU32BE(buf + 20);
        std::memcpy(m.stock, buf + 24, 8); m.stock[8] = '\0';
        m.price = ReadPrice4(buf + 32);
        if (cbs.on_add) cbs.on_add(m);
        return 36;
    }
    case 'F': {
        if (len < 40) return 0;
        MsgAdd m{};
        m.ts_ns = ReadNanos(buf + 5);
        m.order_ref = ReadU64BE(buf + 11);
        m.side = (buf[19] == 'B') ? Side::Buy : Side::Sell;
        m.shares = ReadU32BE(buf + 20);
        std::memcpy(m.stock, buf + 24, 8); m.stock[8] = '\0';
        m.price = ReadPrice4(buf + 32);
        if (cbs.on_add) cbs.on_add(m);
        return 40;
    }
    case 'E': {
        if (len < 31) return 0;
        MsgExecuted m{};
        m.ts_ns = ReadNanos(buf + 5);
        m.order_ref = ReadU64BE(buf + 11);
        m.shares = ReadU32BE(buf + 19);
        if (cbs.on_executed) cbs.on_executed(m);
        return 31;
    }
    case 'X': {
        if (len < 23) return 0;
        MsgCancelled m{};
        m.ts_ns = ReadNanos(buf + 5);
        m.order_ref = ReadU64BE(buf + 11);
        m.shares = ReadU32BE(buf + 19);
        if (cbs.on_cancelled) cbs.on_cancelled(m);
        return 23;
    }
    case 'D': {
        if (len < 19) return 0;
        MsgDeleted m{};
        m.ts_ns = ReadNanos(buf + 5);
        m.order_ref = ReadU64BE(buf + 11);
        if (cbs.on_deleted) cbs.on_deleted(m);
        return 19;
    }
    case 'U': {
        if (len < 35) return 0;
        MsgReplaced m{};
        m.ts_ns = ReadNanos(buf + 5);
        m.old_ref = ReadU64BE(buf + 11);
        m.new_ref = ReadU64BE(buf + 19);
        m.shares = ReadU32BE(buf + 27);
        m.price = ReadPrice4(buf + 31);
        if (cbs.on_replaced) cbs.on_replaced(m);
        return 35;
    }
    default: return 0;
    }
}

std::size_t ITCHParser::ParseFile(const std::string& path, const ITCHCallbacks& cbs) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) throw std::runtime_error("Cannot open ITCH file: " + path);

    struct stat st{};
    ::fstat(fd, &st);
    const std::size_t file_size = static_cast<std::size_t>(st.st_size);

    if (file_size == 0) { ::close(fd); return 0; }

    void* raw = ::mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (raw == MAP_FAILED) throw std::runtime_error("mmap failed for: " + path);

    ::madvise(raw, file_size, MADV_SEQUENTIAL);

    const auto* buf = reinterpret_cast<const uint8_t*>(raw);
    std::size_t pos = 0;
    std::size_t count = 0;

    while (pos + 2 <= file_size) {
        uint16_t msg_len = (uint16_t(buf[pos]) << 8) | buf[pos + 1];
        pos += 2;
        if (pos + msg_len > file_size) break;

        std::size_t consumed = ParseMessage(buf + pos, msg_len, cbs);
        (void)consumed;
        pos += msg_len;
        ++count;
    }

    ::munmap(raw, file_size);
    return count;
}

} // namespace chronos

namespace chronos {


SPSCLogger::SPSCLogger(const std::string& path)
    : file_(path) {
    if (!file_.is_open())
        throw std::runtime_error("Cannot open trade log: " + path);

    file_ << "buy_id,sell_id,price,quantity,ts_ns\n";
    file_.flush();

    // Start the drain thread.
    worker_ = std::thread([this]{ DrainLoop(); });
}

SPSCLogger::~SPSCLogger() {
    stop_.store(true, std::memory_order_release);
    if (worker_.joinable()) worker_.join();
    file_.flush();
}

bool SPSCLogger::TryLog(const Trade& t) noexcept {
    bool ok = ring_.TryPush(t);
    if (!ok) dropped_.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

void SPSCLogger::Flush() {
    // Spin until the consumer has drained the ring.
    while (!ring_.Empty())
        std::this_thread::yield();
    file_.flush();
}

void SPSCLogger::DrainLoop() {
    // Logger thread does not need a real-time core – keep it off Core 0/1.
    Trade t;
    while (true) {
        bool worked = false;
        while (ring_.TryPop(t)) {
            file_ << t.buy_id  << ','
                  << t.sell_id << ','
                  << t.price   << ','
                  << t.quantity << ','
                  << t.ts_ns   << '\n';
            worked = true;
        }
        if (!worked) {
            if (stop_.load(std::memory_order_acquire)) break;
            // Brief pause avoids burning 100% CPU on an idle logger.
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }
    // Final drain after stop signal.
    while (ring_.TryPop(t)) {
        file_ << t.buy_id  << ','
              << t.sell_id << ','
              << t.price   << ','
              << t.quantity << ','
              << t.ts_ns   << '\n';
    }
}


} // namespace chronos
