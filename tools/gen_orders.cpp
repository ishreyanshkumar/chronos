// ─────────────────────────────────────────────────────────────────────────────
// Chronos :: gen_orders
//
// Generates a synthetic multi-million-row CSV of orders with engineered edge
// cases for stress-testing the matching engine.
//
// Output format: id,side,type,price,quantity,ts_ns
// ─────────────────────────────────────────────────────────────────────────────
#include "chronos.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>

static void usage(const char* prog) {
    std::printf("Usage: %s <num_orders> [output.csv]\n", prog);
}

int main(int argc, char* argv[]) {
    if (argc < 2) { usage(argv[0]); return 1; }

    const std::size_t n = static_cast<std::size_t>(std::atoll(argv[1]));
    if (n == 0) { std::fprintf(stderr, "Invalid count\n"); return 1; }

    const char* outpath = (argc >= 3) ? argv[2] : "orders.csv";

    FILE* f = std::fopen(outpath, "w");
    if (!f) { std::perror("fopen"); return 1; }

    std::fprintf(f, "id,side,type,price,quantity,ts_ns\n");

    // Deterministic seed for reproducible datasets.
    std::mt19937_64 rng(0xC4A0B1E5D6F2C8A3ULL);
    std::uniform_int_distribution<int>      side_dist(0, 1);
    std::uniform_int_distribution<int>      type_dist(0, 9); // 80% limit, 10% mkt, 10% cancel
    std::normal_distribution<double>        price_dist(100.0, 0.50); // $100 ± $0.50
    std::uniform_int_distribution<uint32_t> qty_dist(100, 10000);

    using namespace chronos;

    const uint64_t base_ts = NowNanos();
    chronos::OrderId next_cancel_id = 0; // ID of a recently added order to cancel

    for (std::size_t i = 0; i < n; ++i) {
        chronos::OrderId id = static_cast<chronos::OrderId>(i + 1);
        const uint64_t ts   = base_ts + i * 100; // 100 ns apart

        int type_roll = type_dist(rng);
        const char* type_str;
        chronos::Price price;
        uint32_t qty;

        if (type_roll <= 7) {
            // Limit order (80%)
            type_str = "L";
            double p = price_dist(rng);
            if (p <= 0) p = 0.01;
            price = static_cast<chronos::Price>(p * 10000);
            qty   = qty_dist(rng);
            next_cancel_id = id; // remember for future cancel
        } else if (type_roll == 8) {
            // Market order (10%)
            type_str = "M";
            price    = 0;
            qty      = qty_dist(rng);
        } else {
            // Cancel a previously seen order (10%)
            type_str = "C";
            price    = 0;
            qty      = 0;
            id       = (next_cancel_id > 0) ? next_cancel_id : 1;
            next_cancel_id = 0;
        }

        const char* side_str = (side_dist(rng) == 0) ? "B" : "S";

        // Edge cases every 10000 orders
        if (i % 10000 == 0) {
            // Crossed market: buy at ask+1 tick (guaranteed immediate fill)
            std::fprintf(f, "%lu,%s,L,%ld,%u,%lu\n",
                         id, "S", (chronos::Price)(100'0000 - 100), 100, ts);
            ++i; id++;
        }

        std::fprintf(f, "%lu,%s,%s,%ld,%u,%lu\n",
                     id, side_str, type_str, price, qty, ts);
    }

    std::fclose(f);
    std::printf("Generated %zu orders → %s\n", n, outpath);
    return 0;
}
