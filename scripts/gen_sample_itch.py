#!/usr/bin/env python3
"""
Chronos :: gen_sample_itch.py

Generates a minimal NASDAQ TotalView-ITCH 5.0 binary file for integration
testing.  The file contains Add Order (A), Execute (E), and Delete (D)
messages that can be replayed with itch_replay or chronos_engine replay.

ITCH 5.0 wire format: each message is preceded by a 2-byte big-endian length.

Output: data/sample.itch
"""
import struct
import os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data", "sample.itch")
os.makedirs(os.path.dirname(OUT), exist_ok=True)


def u16be(v):  return struct.pack(">H", v)
def u32be(v):  return struct.pack(">I", v)
def u48be(v):  return struct.pack(">Q", v)[2:]  # 6 bytes
def u64be(v):  return struct.pack(">Q", v)


def msg_add(order_ref, side, shares, stock, price_x10000, ts_ns=0, locate=1, tracking=0):
    """
    ITCH 5.0 'A' (Add Order no MPID) = 36 bytes:
      1  msg_type
      2  stock_locate
      2  tracking_number
      6  timestamp (ns since midnight)
      8  order_reference_number
      1  buy_sell_indicator
      4  shares
      8  stock
      4  price
    = 36 bytes
    """
    body = (
        b'A'
        + u16be(locate)
        + u16be(tracking)
        + u48be(ts_ns)
        + u64be(order_ref)
        + side.encode()
        + u32be(shares)
        + stock.ljust(8).encode()[:8]
        + u32be(price_x10000)
    )
    assert len(body) == 36, f"Add body len={len(body)} expected 36"
    return u16be(len(body)) + body


def msg_execute(order_ref, shares, match_number=1, ts_ns=0, locate=1, tracking=0):
    """
    ITCH 5.0 'E' (Order Executed) = 30 bytes:
      1  msg_type
      2  stock_locate
      2  tracking_number
      6  timestamp
      8  order_reference_number
      4  executed_shares
      8  match_number
    = 31 bytes
    """
    body = (
        b'E'
        + u16be(locate)
        + u16be(tracking)
        + u48be(ts_ns)
        + u64be(order_ref)
        + u32be(shares)
        + u64be(match_number)
    )
    assert len(body) == 31, f"Exec body len={len(body)} expected 31"
    return u16be(len(body)) + body


def msg_delete(order_ref, ts_ns=0, locate=1, tracking=0):
    """
    ITCH 5.0 'D' (Order Deleted) = 19 bytes:
      1  msg_type
      2  stock_locate
      2  tracking_number
      6  timestamp
      8  order_reference_number
    = 19 bytes
    """
    body = (
        b'D'
        + u16be(locate)
        + u16be(tracking)
        + u48be(ts_ns)
        + u64be(order_ref)
    )
    assert len(body) == 19, f"Delete body len={len(body)} expected 19"
    return u16be(len(body)) + body


def main():
    messages = []
    ts = 34_200_000_000_000  # ~9:30 AM in nanoseconds since midnight

    # Add 100 sell limit orders at $100.00-$100.99 for AAPL
    for i in range(100):
        ref = 1000 + i
        price = 100_0000 + i * 100  # $100.0000 + i*$0.0100
        messages.append(msg_add(ref, 'S', 100, 'AAPL', price, ts + i * 1_000))

    # Add 99 buy limit orders at $99.01-$99.99 (resting bids, no cross)
    for i in range(99):
        ref = 2000 + i
        price = 99_0100 + i * 100
        messages.append(msg_add(ref, 'B', 100, 'AAPL', price, ts + 200_000 + i * 1_000))

    # Aggressive buy that crosses the spread - matches sell at $100.00 (ref 1000)
    messages.append(msg_add(3000, 'B', 200, 'AAPL', 100_0200, ts + 400_000))

    # Execute the matched sell
    messages.append(msg_execute(1000, 200, match_number=1, ts_ns=ts + 400_100))

    # Delete a few resting orders
    for ref in [1001, 1002, 1003, 2000, 2001]:
        messages.append(msg_delete(ref, ts_ns=ts + 500_000))

    with open(OUT, "wb") as f:
        for m in messages:
            f.write(m)

    total_bytes = sum(len(m) for m in messages)
    print(f"Generated {len(messages)} ITCH messages ({total_bytes} bytes) -> {OUT}")


if __name__ == "__main__":
    main()
