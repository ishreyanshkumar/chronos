// ─────────────────────────────────────────────────────────────────────────────
// Chronos :: Latency Benchmark
//
// Measures per-operation latency distributions (p50, p90, p99, p99.9) for:
//   1. AddOrder (limit, resting)
//   2. Match (crossing buy + sell)
//   3. CancelOrder
//
// Compiles in two modes:
//   CHRONOS_STANDALONE_BENCH – uses a built-in stat library (no external deps)
//   default                   – uses Google Benchmark
// ─────────────────────────────────────────────────────────────────────────────
#include "order_book.hpp"
#include "cpu_utils.hpp"
#include "memory_arena.hpp"
#include "spsc_ring.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <numeric>
#include <string>

#ifndef CHRONOS_STANDALONE_BENCH
  #include <benchmark/benchmark.h>
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Standalone percentile benchmark (no Google Benchmark dependency)
// ─────────────────────────────────────────────────────────────────────────────
#ifdef CHRONOS_STANDALONE_BENCH

static constexpr std::size_t kWarmup    = 50'000;
static constexpr std::size_t kMeasure   = 2'000'000;
static constexpr std::size_t kTotal     = kWarmup + kMeasure;

struct Stats {
    uint64_t min_ns, p50, p90, p99, p999, max_ns;
    double   mean_ns;
    uint64_t ops;
};

static Stats ComputeStats(std::vector<uint64_t>& samples) {
    std::sort(samples.begin(), samples.end());
    std::size_t n = samples.size();
    Stats s{};
    s.ops    = n;
    s.min_ns = samples.front();
    s.max_ns = samples.back();
    s.p50    = samples[n * 50 / 100];
    s.p90    = samples[n * 90 / 100];
    s.p99    = samples[n * 99 / 100];
    s.p999   = samples[n * 999 / 1000];
    s.mean_ns = static_cast<double>(
        std::accumulate(samples.begin(), samples.end(), uint64_t{0})) / n;
    return s;
}

static void PrintStats(const std::string& name, const Stats& s) {
    std::printf("\n── %s ──\n", name.c_str());
    std::printf("  Samples : %lu\n", s.ops);
    std::printf("  Min     : %lu ns\n", s.min_ns);
    std::printf("  Mean    : %.1f ns\n", s.mean_ns);
    std::printf("  p50     : %lu ns\n", s.p50);
    std::printf("  p90     : %lu ns\n", s.p90);
    std::printf("  p99     : %lu ns\n", s.p99);
    std::printf("  p99.9   : %lu ns\n", s.p999);
    std::printf("  Max     : %lu ns\n", s.max_ns);
}

// ── Benchmark: AddOrder (limit, no match) ─────────────────────────────────────
static Stats BenchAdd() {
    using namespace chronos;
    OrderBook book;
    std::vector<uint64_t> lat;
    lat.reserve(kMeasure);

    OrderId id = 1;
    // Fill one side so the book has depth.
    for (std::size_t i = 0; i < 1000; ++i)
        book.AddOrder(id++, Side::Sell, OrderType::Limit, 100'5000 + Price(i*100), 100, 0);

    for (std::size_t i = 0; i < kTotal; ++i) {
        Price p = 100'0000 - Price(i % 500 + 1) * 100; // below best ask
        CompilerFence();
        uint64_t t0 = NowNanos();
        book.AddOrder(id++, Side::Buy, OrderType::Limit, p, 100, 0);
        uint64_t t1 = NowNanos();
        CompilerFence();
        if (i >= kWarmup) lat.push_back(t1 - t0);
    }
    return ComputeStats(lat);
}

// ── Benchmark: Match (crossing order) ────────────────────────────────────────
static Stats BenchMatch() {
    using namespace chronos;
    std::vector<uint64_t> lat;
    lat.reserve(kMeasure);

    OrderId id = 1;
    OrderBook book;
    for (std::size_t i = 0; i < kTotal; ++i) {
        book.AddOrder(id, Side::Sell, OrderType::Limit, 100'0000, 100, 0);

        CompilerFence();
        uint64_t t0 = NowNanos();
        book.AddOrder(id + 1, Side::Buy, OrderType::Limit, 100'0000, 100, 0);
        uint64_t t1 = NowNanos();
        CompilerFence();

        id += 2;
        if (i >= kWarmup) lat.push_back(t1 - t0);
    }
    return ComputeStats(lat);
}

// ── Benchmark: CancelOrder ────────────────────────────────────────────────────
static Stats BenchCancel() {
    using namespace chronos;
    std::vector<uint64_t> lat;
    lat.reserve(kMeasure);

    // Pre-populate a book; measure cancel latency.
    OrderBook book;
    std::vector<OrderId> ids;
    ids.reserve(kTotal);

    for (std::size_t i = 0; i < kTotal; ++i) {
        OrderId oid = static_cast<OrderId>(i + 1);
        ids.push_back(oid);
        book.AddOrder(oid, Side::Buy, OrderType::Limit,
                      100'0000 - Price(i % 1000) * 10, 100, 0);
    }

    for (std::size_t i = 0; i < kTotal; ++i) {
        CompilerFence();
        uint64_t t0 = NowNanos();
        book.CancelOrder(ids[i]);
        uint64_t t1 = NowNanos();
        CompilerFence();
        if (i >= kWarmup) lat.push_back(t1 - t0);
    }
    return ComputeStats(lat);
}

// ── Throughput benchmark ──────────────────────────────────────────────────────
static void BenchThroughput(std::size_t n) {
    using namespace chronos;
    OrderBook book;
    OrderId id = 1;

    uint64_t t0 = NowNanos();
    for (std::size_t i = 0; i < n; ++i) {
        Price p  = 100'0000 + Price((i % 201) - 100);
        Side  s  = (i % 2 == 0) ? Side::Buy : Side::Sell;
        book.AddOrder(id++, s, OrderType::Limit, p, 100, 0);
    }
    uint64_t t1 = NowNanos();

    double ops_s = static_cast<double>(n) / (static_cast<double>(t1 - t0) * 1e-9);
    std::printf("\n── Throughput ──\n");
    std::printf("  Orders  : %zu\n", n);
    std::printf("  Elapsed : %.3f s\n", static_cast<double>(t1 - t0) * 1e-9);
    std::printf("  Rate    : %.2f M orders/s\n", ops_s / 1e6);
}

int main() {
    std::printf("╔══════════════════════════════════════════════╗\n");
    std::printf("║   Chronos HFT Engine – Latency Benchmark     ║\n");
    std::printf("╚══════════════════════════════════════════════╝\n");
    std::printf("Warmup: %zu  |  Measure: %zu samples\n", kWarmup, kMeasure);

    auto s_add    = BenchAdd();    PrintStats("AddOrder (resting limit)", s_add);
    auto s_match  = BenchMatch();  PrintStats("AddOrder (crossing → fill)", s_match);
    auto s_cancel = BenchCancel(); PrintStats("CancelOrder", s_cancel);
    BenchThroughput(10'000'000);

    std::printf("\n[Target] p50 < 80 ns  |  throughput > 20 M/s\n\n");
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Google Benchmark mode
// ─────────────────────────────────────────────────────────────────────────────
#else // !CHRONOS_STANDALONE_BENCH

using namespace chronos;

// ==========================================
// BASELINE FOR COMPARISON: NAIVE STL ENGINE
// ==========================================
#include <list>
#include <unordered_map>
#include <map>

struct STLOrder {
    uint64_t orderId;
    uint64_t price;
    uint32_t quantity;
    Side side;
};

class LimitOrderBookSTL {
private:
    std::map<uint64_t, std::list<STLOrder>, std::greater<uint64_t>> bids;
    std::map<uint64_t, std::list<STLOrder>> asks;
    std::unordered_map<uint64_t, std::list<STLOrder>::iterator> orderMap;

public:
    int AddOrder(uint64_t orderId, Side side, OrderType type, uint64_t price, uint32_t quantity, uint64_t ts) {
        int fills = 0;
        if (side == Side::Buy) {
            auto it = asks.begin();
            while (quantity > 0 && it != asks.end() && it->first <= price) {
                auto& list = it->second;
                auto listIt = list.begin();
                while (quantity > 0 && listIt != list.end()) {
                    uint32_t tradeQty = std::min(quantity, listIt->quantity);
                    quantity -= tradeQty;
                    listIt->quantity -= tradeQty;
                    fills++;
                    if (listIt->quantity == 0) {
                        orderMap.erase(listIt->orderId);
                        listIt = list.erase(listIt);
                    } else {
                        ++listIt;
                    }
                }
                if (list.empty()) {
                    it = asks.erase(it);
                } else {
                    ++it;
                }
            }
            if (quantity > 0 && type == OrderType::Limit) {
                bids[price].push_back({orderId, price, quantity, side});
                orderMap[orderId] = std::prev(bids[price].end());
            }
        } else {
            auto it = bids.begin();
            while (quantity > 0 && it != bids.end() && it->first >= price) {
                auto& list = it->second;
                auto listIt = list.begin();
                while (quantity > 0 && listIt != list.end()) {
                    uint32_t tradeQty = std::min(quantity, listIt->quantity);
                    quantity -= tradeQty;
                    listIt->quantity -= tradeQty;
                    fills++;
                    if (listIt->quantity == 0) {
                        orderMap.erase(listIt->orderId);
                        listIt = list.erase(listIt);
                    } else {
                        ++listIt;
                    }
                }
                if (list.empty()) {
                    it = bids.erase(it);
                } else {
                    ++it;
                }
            }
            if (quantity > 0 && type == OrderType::Limit) {
                asks[price].push_back({orderId, price, quantity, side});
                orderMap[orderId] = std::prev(asks[price].end());
            }
        }
        return fills;
    }

    bool CancelOrder(uint64_t orderId) {
        auto mapIt = orderMap.find(orderId);
        if (mapIt == orderMap.end()) return false;
        
        auto listIt = mapIt->second;
        uint64_t price = listIt->price;
        Side side = listIt->side;
        
        if (side == Side::Buy) {
            auto& list = bids[price];
            list.erase(listIt);
            if (list.empty()) bids.erase(price);
        } else {
            auto& list = asks[price];
            list.erase(listIt);
            if (list.empty()) asks.erase(price);
        }
        orderMap.erase(mapIt);
        return true;
    }
};

static void BM_Baseline_STL(benchmark::State& state) {
    const int N = static_cast<int>(state.range(0));
    LimitOrderBookSTL book;
    uint64_t id = 1;
    for (auto _ : state) {
        for (int i = 0; i < N; ++i) {
            benchmark::DoNotOptimize(
                book.AddOrder(id + i, Side::Buy, OrderType::Limit,
                              10000 - ((id + i) % 500), 100, 0));
        }
        state.PauseTiming();
        for (int i = 0; i < N; ++i) book.CancelOrder(id + i);
        state.ResumeTiming();
        
        id = (id + N) % 1'000'000 + 1; // Bound ID map size
    }
    state.SetItemsProcessed(state.iterations() * N);
}
BENCHMARK(BM_Baseline_STL)->Arg(1000)->Arg(10000)->Arg(100000);

static void BM_AddOrderResting(benchmark::State& state) {
    const int N = static_cast<int>(state.range(0));
    OrderBook book;
    OrderId   id = 1;
    for (auto _ : state) {
        for (int i = 0; i < N; ++i) {
            benchmark::DoNotOptimize(
                book.AddOrder(id + i, Side::Buy, OrderType::Limit,
                              10000 - Price((id + i) % 500), 100, 0));
        }
        state.PauseTiming();
        for (int i = 0; i < N; ++i) book.CancelOrder(id + i);
        state.ResumeTiming();
        
        id = (id + N) % 1'000'000 + 1; // Bound ID map size
    }
    state.SetItemsProcessed(state.iterations() * N);
}
BENCHMARK(BM_AddOrderResting)->Arg(1000)->Arg(10000)->Arg(100000);

static void BM_MatchCross(benchmark::State& state) {
    const int N = static_cast<int>(state.range(0));
    OrderId id = 1;
    OrderBook book;
    for (auto _ : state) {
        for (int i = 0; i < N; ++i) {
            book.AddOrder(id, Side::Sell, OrderType::Limit, 10000, 100, 0);
            benchmark::DoNotOptimize(
                book.AddOrder(id + 1, Side::Buy,  OrderType::Limit, 10000, 100, 0));
            id += 2;
        }
        id = (id % 1'000'000) + 1; // Bound ID map size
    }
    state.SetItemsProcessed(state.iterations() * N);
}
BENCHMARK(BM_MatchCross)->Arg(1000)->Arg(10000)->Arg(100000);

static void BM_Cancel(benchmark::State& state) {
    const int N = static_cast<int>(state.range(0));
    OrderBook book;
    std::vector<OrderId> ids;
    ids.reserve(N);
    for (OrderId i = 1; i <= static_cast<OrderId>(N); ++i) ids.push_back(i);
    
    for (auto _ : state) {
        state.PauseTiming();
        for (int i = 0; i < N; ++i) {
            book.AddOrder(ids[i], Side::Buy, OrderType::Limit, 10000 - Price(ids[i]%1000), 100, 0);
        }
        state.ResumeTiming();
        
        for (int i = 0; i < N; ++i) {
            benchmark::DoNotOptimize(book.CancelOrder(ids[i]));
        }
    }
    state.SetItemsProcessed(state.iterations() * N);
}
BENCHMARK(BM_Cancel)->Arg(1000)->Arg(10000);

static void BM_MarketOrder(benchmark::State& state) {
    const int levels = static_cast<int>(state.range(0));
    OrderBook book;
    for (auto _ : state) {
        state.PauseTiming();
        for (int l = 0; l < levels; ++l)
            book.AddOrder(l + 1, Side::Sell, OrderType::Limit, 10000 + l, 100, 0);
        state.ResumeTiming();
        
        benchmark::DoNotOptimize(book.AddOrder(levels + 1, Side::Buy, OrderType::Market, 0, levels * 100, 0));
    }
    state.SetItemsProcessed(state.iterations() * levels);
}
BENCHMARK(BM_MarketOrder)->Arg(10)->Arg(100)->Arg(500);

static void BM_ArenaAllocDealloc(benchmark::State& state) {
    const size_t N = static_cast<size_t>(state.range(0));
    MemoryArena<Order, 1 << 22> arena;
    std::vector<Order*> ptrs(N);
    for (auto _ : state) {
        for (size_t i = 0; i < N; ++i) ptrs[i] = arena.Alloc();
        for (size_t i = 0; i < N; ++i) arena.Free(ptrs[i]);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(N) * 2);
}
BENCHMARK(BM_ArenaAllocDealloc)->Arg(1000)->Arg(10000)->Arg(100000);

static void BM_SPSCThroughput(benchmark::State& state) {
    SPSCRing<Trade, 4096> ring;
    Trade t{};
    t.buy_id = 1; t.sell_id = 2; t.price = 10000; t.quantity = 100;
    Trade popped{};
    for (auto _ : state) {
        for (int i = 0; i < 1000; ++i) {
            ring.TryPush(t);
            ring.TryPop(popped);
            benchmark::DoNotOptimize(popped);
        }
    }
    state.SetItemsProcessed(state.iterations() * 1000);
}
BENCHMARK(BM_SPSCThroughput);

#endif // CHRONOS_STANDALONE_BENCH
