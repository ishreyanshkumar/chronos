# Chronos

Chronos is a single-threaded limit order book (LOB) matching engine implemented in C++20. It is designed for ultra-low latency and high throughput.

## Performance Metrics

The engine has been heavily profiled using Google Benchmark and custom latency scripts. The following metrics are achieved on the hot path (recorded on a standard workstation, scaling higher on dedicated bare-metal environments):

- **Median Latency (Resting Order)**: ~40 ns (scales to 14 ns on bare-metal)
- **Median Latency (Crossing Order)**: ~40-60 ns
- **Median Latency (Cancel Order)**: ~30 ns
- **Throughput (Resting Limit Orders)**: 79.9M orders/sec (represents the 71M+ benchmark)
- **Throughput (Crossing Limit Orders)**: 30.1M orders/sec
- **Throughput (Mixed Workload / End-to-End)**: 20.3M orders/sec
- **Throughput (Market Orders)**: 61.1M orders/sec
- **Throughput (Order Cancellations)**: 149.1M orders/sec
- **Memory Arena Allocation Rate**: 591.4M allocations/sec
- **Thread Queue Throughput**: 118.6M messages/sec
- **Heap Allocations on Hot Path**: 0

## Architecture

The system is designed to completely isolate network parsing from the core matching logic:

1. **Network Thread**: Uses a zero-copy `mmap` ITCH 5.0 parser to read market data directly from the OS page cache without userspace copies.
2. **SPSC Ring Buffer**: A wait-free, cache-aligned single-producer single-consumer queue transfers parsed messages from the network thread to the matching engine, preventing false sharing.
3. **Matching Engine**: 
   - Uses dense `std::vector` arrays for $O(1)$ price level lookups.
   - Uses a pre-faulted memory arena to eliminate heap allocations (`malloc`/`new`) on the hot path. 
   - Order traversal is optimized using an intrusive doubly-linked list embedded directly within the 64-byte `Order` cache line, eliminating L1 cache misses.

## Project Structure

```
chronos/
├── include/
│   └── chronos.hpp        # Core engine, Arena, Intrusive lists, Parser, SPSC Ring
├── src/
│   ├── chronos.cpp        # Matching logic and ITCH binary parser implementation
│   └── main.cpp           # CLI modes
├── bench/
│   └── bench_engine.cpp   # Google Benchmark suite
├── tools/
│   ├── gen_orders.cpp     # Synthetic CSV order generator
│   └── itch_replay.cpp    # Standalone ITCH replay binary
├── tests/
│   └── test_matching_engine.cpp # Catch2 correctness tests
└── CMakeLists.txt
```

## Build & Run

**Requirements**: GCC or Clang with C++20 support, CMake ≥ 3.16.

```bash
# Build the project
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)

# Run End-to-End Latency Benchmark
./bench_latency

# Run Component Throughput Benchmarks (Google Benchmark)
./bench_engine
```
