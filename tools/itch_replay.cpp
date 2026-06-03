// ─────────────────────────────────────────────────────────────────────────────
// Chronos :: itch_replay
//
// Standalone ITCH 5.0 file replay tool.
// Reads a binary ITCH file and feeds it through the matching engine,
// printing top-of-book updates and fill statistics.
// ─────────────────────────────────────────────────────────────────────────────
#include "order_book.hpp"
#include "itch_parser.hpp"
#include "spsc_logger.hpp"
#include "cpu_utils.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <optional>

static void usage(const char* prog) {
    std::printf("Usage: %s <itch_file> [symbol]\n"
                "  symbol  – 8-char NASDAQ stock symbol (optional filter)\n",
                prog);
}

int main(int argc, char* argv[]) {
    if (argc < 2) { usage(argv[0]); return 1; }

    const std::string path = argv[1];
    std::optional<std::string> symbol;
    if (argc >= 3) symbol = argv[2];

    using namespace chronos;

    SPSCLogger logger("trades_itch.csv");

    uint64_t fills = 0;
    uint64_t adds  = 0;
    uint64_t dels  = 0;

    OrderBook book([&](const Trade& t){ (void)logger.TryLog(t); ++fills; });

    ITCHCallbacks cbs;

    cbs.on_add = [&](const MsgAdd& m) {
        if (symbol) {
            std::string stk(m.stock, 8);
            if (stk.find(*symbol) == std::string::npos) return;
        }
        book.AddOrder(m.order_ref, m.side, OrderType::Limit,
                      m.price, m.shares, m.ts_ns);
        ++adds;
    };

    cbs.on_deleted = [&](const MsgDeleted& m) {
        book.CancelOrder(m.order_ref);
        ++dels;
    };

    cbs.on_cancelled = [&](const MsgCancelled& m) {
        book.CancelOrder(m.order_ref);
    };

    cbs.on_replaced = [&](const MsgReplaced& m) {
        book.CancelOrder(m.old_ref);
        book.AddOrder(m.new_ref, Side::Buy,
                      OrderType::Limit, m.price, m.shares, m.ts_ns);
    };

    const uint64_t t0 = NowNanos();
    std::size_t total = ITCHParser{}.ParseFile(path, cbs);
    const uint64_t t1 = NowNanos();

    logger.Flush();

    double elapsed = static_cast<double>(t1 - t0) * 1e-9;

    std::printf("\n");
    std::printf("File    : %s\n", path.c_str());
    std::printf("Symbol  : %s\n", symbol ? symbol->c_str() : "(all)");
    std::printf("Total   : %zu messages\n", total);
    std::printf("Adds    : %lu\n", adds);
    std::printf("Cancels : %lu\n", dels);
    std::printf("Fills   : %lu\n", fills);
    std::printf("Resting : %zu orders\n", book.OrderCount());
    std::printf("Elapsed : %.3f s  (%.2f M msgs/s)\n",
                elapsed, static_cast<double>(total) / elapsed / 1e6);

    auto bid = book.BestBid();
    auto ask = book.BestAsk();
    if (bid) std::printf("Best Bid: $%.4f\n", static_cast<double>(*bid) / 10000.0);
    if (ask) std::printf("Best Ask: $%.4f\n", static_cast<double>(*ask) / 10000.0);

    return 0;
}
