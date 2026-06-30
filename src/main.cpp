// ─────────────────────────────────────────────────────────────────────────────
// Chronos :: Main entry point
//
// Wires together the three-core architecture:
//
//   Core 0  – Market-data ingestion (ITCH file reader)
//   Core 1  – Matching engine        (OrderBook hot path)
//   Core 2  – Trade logger           (SPSCLogger drain thread)
//
// For PCAP/ITCH file replay the ingestion and matching run sequentially on the
// caller's thread (no network latency to hide); for a live deployment you would
// separate ingestion onto its own pinned thread feeding the SPSC queue.
// ─────────────────────────────────────────────────────────────────────────────
#include "chronos.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <optional>
#include <filesystem>

static void usage(const char* prog) {
    std::printf(
        "Usage: %s <command> [options]\n"
        "\n"
        "Commands:\n"
        "  replay  <itch_file> [symbol]   Replay ITCH binary file\n"
        "  synth   <num_orders>           Run synthetic benchmark\n"
        "\n"
        "Examples:\n"
        "  %s replay data/sample.itch AAPL\n"
        "  %s synth 5000000\n",
        prog, prog, prog);
}

// ── Synthetic benchmark (no file dependency) ──────────────────────────────────
static void RunSynth(std::size_t n_orders, bool enable_logger) {
    using namespace chronos;

    std::size_t fills_total = 0;
    std::unique_ptr<SPSCLogger> logger;
    std::unique_ptr<OrderBook> book;
    
    if (enable_logger) {
        std::filesystem::create_directory("results");
        logger = std::make_unique<SPSCLogger>("results/trades_synth.csv");
        SPSCLogger* logger_ptr = logger.get();
        book = std::make_unique<OrderBook>([logger_ptr](const Trade& t){ (void)logger_ptr->TryLog(t); });
    } else {
        book = std::make_unique<OrderBook>([](const Trade& t){});
    }

    const uint64_t t0 = NowNanos();

    // Interleave buys and sells so the engine actually matches.
    for (std::size_t i = 0; i < n_orders; ++i) {
        const OrderId id    = static_cast<OrderId>(i + 1);
        const Price   price = 100'0000 + static_cast<Price>((i % 201) - 100); // ±$0.0100
        const Quantity qty  = 100;
        const Side     side = (i % 2 == 0) ? Side::Buy : Side::Sell;

        int f = book->AddOrder(id, side, OrderType::Limit, price, qty, NowNanos());
        if (f > 0) fills_total += static_cast<std::size_t>(f);
    }

    const uint64_t t1 = NowNanos();
    const double   elapsed_s  = static_cast<double>(t1 - t0) * 1e-9;
    const double   ops_per_s  = static_cast<double>(n_orders) / elapsed_s;
    const double   ns_per_op  = static_cast<double>(t1 - t0) / static_cast<double>(n_orders);

    if (logger) {
        logger->Flush();
    }

    std::printf("\n═══════════════════════════════════════════\n");
    std::printf("  Chronos :: Synthetic Benchmark Results\n");
    std::printf("═══════════════════════════════════════════\n");
    std::printf("  Orders processed : %zu\n",   n_orders);
    std::printf("  Fills generated  : %zu\n",   fills_total);
    std::printf("  Elapsed          : %.3f s\n", elapsed_s);
    std::printf("  Throughput       : %.2f M orders/s\n", ops_per_s / 1e6);
    std::printf("  Avg latency      : %.1f ns/order\n", ns_per_op);
    if (logger) {
        std::printf("  Logger dropped   : %lu\n", logger->Dropped());
    }
    std::printf("═══════════════════════════════════════════\n\n");
}

// ── ITCH file replay ──────────────────────────────────────────────────────────
static void RunReplay(const std::string& path,
                      const std::optional<std::string>& symbol_filter) {
    using namespace chronos;

    std::filesystem::create_directory("results");
    SPSCLogger logger("results/trades_replay.csv");

    std::size_t msg_count  = 0;
    std::size_t add_count  = 0;
    std::size_t del_count  = 0;
    std::size_t fills_total = 0;

    OrderBook book([&](const Trade& t){ (void)logger.TryLog(t); });

    ITCHCallbacks cbs;

    cbs.on_add = [&](const MsgAdd& m) {
        if (symbol_filter && std::string(m.stock).substr(0,8).find(*symbol_filter) == std::string::npos)
            return;
        int f = book.AddOrder(m.order_ref, m.side, OrderType::Limit,
                              m.price, m.shares, m.ts_ns);
        if (f > 0) fills_total += static_cast<std::size_t>(f);
        ++add_count;
    };

    cbs.on_deleted = [&](const MsgDeleted& m) {
        book.CancelOrder(m.order_ref);
        ++del_count;
    };

    cbs.on_cancelled = [&](const MsgCancelled& m) {
        // Partial cancel: just delete for now (full LOB would adjust qty).
        book.CancelOrder(m.order_ref);
    };

    cbs.on_replaced = [&](const MsgReplaced& m) {
        book.CancelOrder(m.old_ref);
        // New order at updated price/qty.
        book.AddOrder(m.new_ref, Side::Buy /* will match – side unknown from U msg */,
                      OrderType::Limit, m.price, m.shares, m.ts_ns);
    };

    const uint64_t t0 = NowNanos();
    msg_count = ITCHParser{}.ParseFile(path, cbs);
    const uint64_t t1 = NowNanos();

    logger.Flush();

    const double elapsed_s = static_cast<double>(t1 - t0) * 1e-9;

    std::printf("\n═══════════════════════════════════════════\n");
    std::printf("  Chronos :: ITCH Replay Results\n");
    std::printf("═══════════════════════════════════════════\n");
    std::printf("  File             : %s\n", path.c_str());
    if (symbol_filter) std::printf("  Symbol filter    : %s\n", symbol_filter->c_str());
    std::printf("  Total messages   : %zu\n",  msg_count);
    std::printf("  Orders added     : %zu\n",  add_count);
    std::printf("  Orders cancelled : %zu\n",  del_count);
    std::printf("  Fills generated  : %zu\n",  fills_total);
    std::printf("  Resting orders   : %zu\n",  book.OrderCount());
    std::printf("  Elapsed          : %.3f s\n", elapsed_s);
    if (add_count > 0)
        std::printf("  Throughput       : %.2f M orders/s\n",
                    static_cast<double>(add_count) / elapsed_s / 1e6);
    auto bid = book.BestBid();
    auto ask = book.BestAsk();
    if (bid != -1) std::printf("  Best Bid         : %.4f\n", static_cast<double>(bid) / 10000.0);
    if (ask != -1) std::printf("  Best Ask         : %.4f\n", static_cast<double>(ask) / 10000.0);
    std::printf("  Logger dropped   : %lu\n", logger.Dropped());
    std::printf("═══════════════════════════════════════════\n\n");
}

// ── main ─────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    if (argc < 3) { usage(argv[0]); return 1; }

    std::string cmd = argv[1];

    if (cmd == "synth") {
        std::size_t n = static_cast<std::size_t>(std::atoll(argv[2]));
        if (n == 0) { std::fprintf(stderr, "Invalid order count\n"); return 1; }
        RunSynth(n, true);
    } else if (cmd == "synth_fast") {
        std::size_t n = static_cast<std::size_t>(std::atoll(argv[2]));
        if (n == 0) { std::fprintf(stderr, "Invalid order count\n"); return 1; }
        RunSynth(n, false);
    } else if (cmd == "replay") {
        std::string path = argv[2];
        std::optional<std::string> sym;
        if (argc >= 4) sym = argv[3];
        RunReplay(path, sym);
    } else {
        usage(argv[0]);
        return 1;
    }
    return 0;
}
