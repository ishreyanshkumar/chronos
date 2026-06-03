#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "memory_arena.hpp"
#include "order_book.hpp"
#include "price_level.hpp"
#include "spsc_ring.hpp"
#include <vector>

using namespace chronos;

// ── Arena ────────────────────────────────────────────────────
TEST_CASE("Arena alloc/dealloc cycle", "[arena]") {
    MemoryArena<Order, 4> arena;
    REQUIRE(arena.LiveCount() == 0);

    Order* a = arena.Alloc();
    Order* b = arena.Alloc();
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(a != b);
    REQUIRE(arena.LiveCount() == 2);

    arena.Free(a);
    REQUIRE(arena.LiveCount() == 1);

    Order* c = arena.Alloc();
    REQUIRE(c != nullptr);
    REQUIRE(arena.LiveCount() == 2);
}

TEST_CASE("Arena returns nullptr when exhausted", "[arena]") {
    MemoryArena<Order, 2> arena;
    arena.Alloc(); arena.Alloc();
    REQUIRE(arena.Alloc() == nullptr);
}

// ── OrderBook ───────────────────────────────────────────
TEST_CASE("Resting order tracks best bid/ask", "[lob]") {
    OrderBook book;
    book.AddOrder(1, Side::Buy, OrderType::Limit, 9990, 100, 0);
    book.AddOrder(2, Side::Buy, OrderType::Limit, 9995, 100, 0);
    REQUIRE(book.BestBid().value_or(0) == 9995);

    book.AddOrder(3, Side::Sell, OrderType::Limit, 10010, 100, 0);
    book.AddOrder(4, Side::Sell, OrderType::Limit, 10005, 100, 0);
    REQUIRE(book.BestAsk().value_or(0) == 10005);
}

TEST_CASE("Full fill — resting order consumed", "[lob]") {
    std::vector<Trade> trades;
    OrderBook book([&](const Trade& t){ trades.push_back(t); });

    book.AddOrder(1, Side::Sell, OrderType::Limit, 10000, 100, 0);
    book.AddOrder(2, Side::Buy, OrderType::Limit, 10000, 100, 0);

    REQUIRE(trades.size() == 1);
    REQUIRE(trades[0].quantity == 100);
    REQUIRE(book.OrderCount() == 0);
}

TEST_CASE("Partial fill — remainder rests in book", "[lob]") {
    std::vector<Trade> trades;
    OrderBook book([&](const Trade& t){ trades.push_back(t); });

    book.AddOrder(1, Side::Sell, OrderType::Limit, 10000, 50, 0);
    book.AddOrder(2, Side::Buy, OrderType::Limit, 10000, 100, 0);

    REQUIRE(trades.size() == 1);
    REQUIRE(trades[0].quantity == 50);
    REQUIRE(book.OrderCount() == 1);
    REQUIRE(book.BestBid().value_or(0) == 10000);
}

TEST_CASE("Aggressive order sweeps multiple price levels", "[lob]") {
    std::vector<Trade> trades;
    OrderBook book([&](const Trade& t){ trades.push_back(t); });

    book.AddOrder(1, Side::Sell, OrderType::Limit, 10001, 100, 0);
    book.AddOrder(2, Side::Sell, OrderType::Limit, 10002, 100, 0);
    book.AddOrder(3, Side::Sell, OrderType::Limit, 10003, 100, 0);

    book.AddOrder(9, Side::Buy, OrderType::Limit, 10003, 300, 0);

    REQUIRE(trades.size() == 3);
    REQUIRE(book.OrderCount() == 0);
}

TEST_CASE("Price-time priority: oldest order fills first", "[lob]") {
    std::vector<Trade> trades;
    OrderBook book([&](const Trade& t){ trades.push_back(t); });

    book.AddOrder(10, Side::Sell, OrderType::Limit, 10000, 100, 0);
    book.AddOrder(11, Side::Sell, OrderType::Limit, 10000, 100, 0);
    book.AddOrder(99, Side::Buy, OrderType::Limit, 10000, 100, 0);

    REQUIRE(trades.size() == 1);
    REQUIRE(trades[0].sell_id == 10);
}

TEST_CASE("Cancel removes order and recalculates best", "[lob]") {
    OrderBook book;
    book.AddOrder(1, Side::Buy, OrderType::Limit, 9990, 100, 0);
    book.AddOrder(2, Side::Buy, OrderType::Limit, 9995, 100, 0);

    REQUIRE(book.BestBid().value_or(0) == 9995);
    book.CancelOrder(2);
    REQUIRE(book.BestBid().value_or(0) == 9990);
    REQUIRE(book.OrderCount() == 1);
}

TEST_CASE("Cancel non-existent order returns false", "[lob]") {
    OrderBook book;
    REQUIRE(book.CancelOrder(999) == false);
}

TEST_CASE("Market order consumes at best price", "[lob]") {
    std::vector<Trade> trades;
    OrderBook book([&](const Trade& t){ trades.push_back(t); });

    book.AddOrder(1, Side::Sell, OrderType::Limit, 10000, 200, 0);
    book.AddOrder(2, Side::Buy, OrderType::Market, kMarketPrice, 150, 0);

    REQUIRE(trades.size() == 1);
    REQUIRE(trades[0].quantity == 150);
}
