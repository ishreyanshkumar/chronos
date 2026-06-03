# Chronos — Ultra-Low Latency Order Matching Engine

> **Target metrics**: p50 latency < 80 ns · throughput > 20 M orders/s · zero heap allocations on the hot path

---

## Architecture Overview

```
┌──────────────────────────────────────────────────────────────┐
│                     Chronos Architecture                     │
│                                                              │
│  Core 0 (Network / File Reader)                              │
│  ┌─────────────────────┐                                     │
│  │  ITCHParser          │  zero-copy mmap()                  │
│  │  (mmap + MADV_SEQ)  │─────────────────────┐              │
│  └─────────────────────┘                     │              │
│                                              ▼              │
│                                    ┌──────────────────┐     │
│                                    │  SPSCRing<Msg,N> │     │
│                                    │  (wait-free,     │     │
│                                    │   cache-aligned) │     │
│                                    └────────┬─────────┘     │
│                                             │               │
│  Core 1 (Matching Engine)                   ▼               │
│  ┌──────────────────────────────────────────────────────┐   │
│  │  OrderBook                                           │   │
│  │  ┌────────────────┐   ┌────────────────────────┐    │   │
│  │  │ MemoryArena<O> │   │ BidMap / AskMap        │    │   │
│  │  │  (mmap slab,   │   │ (std::map + intrusive  │    │   │
│  │  │   pre-faulted) │   │  doubly-linked list)   │    │   │
│  │  └────────────────┘   └────────────────────────┘    │   │
│  └───────────────────────────────┬──────────────────────┘   │
│                                  │ Trade events              │
│                                  ▼                          │
│                         ┌────────────────┐                  │
│  Core 2 (Logger)        │  SPSCRing<T,N> │                  │
│  ┌──────────────┐       │  (wait-free)   │                  │
│  │  SPSCLogger  │◄──────┴────────────────┘                  │
│  │  (CSV drain) │                                           │
│  └──────────────┘                                           │
└──────────────────────────────────────────────────────────────┘
```

---

## Key Innovations

### 1. Intrusive Doubly-Linked List

Standard LOB implementations allocate list nodes separately from order data. When the matching engine traverses a price level queue, each `->next` pointer dereference causes a **new cache-line load** — a guaranteed L1 cache miss.

Chronos embeds `prev` and `next` directly inside `struct Order`:

```cpp
struct alignas(64) Order {
    OrderId   id;
    Price     price;
    Quantity  quantity, filled;
    // ...
    Order*    prev;   // ← lives in same 64-byte cache line as price/qty
    Order*    next;   // ← brought in for free by the CPU prefetcher
    Nanos     ts_ns;
};
static_assert(sizeof(Order) <= 64);
```

When the CPU loads an `Order` to read its `price`, the `next` pointer arrives in the same cache-line fetch. Queue traversal is effectively free from a memory-traffic perspective.

### 2. Pre-faulted mmap Memory Arena

```cpp
// mmap anonymous, then touch every page at startup
void* raw = mmap(nullptr, TotalBytes(), PROT_READ|PROT_WRITE,
                 MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
madvise(raw, TotalBytes(), MADV_WILLNEED);
for (size_t off = 0; off < TotalBytes(); off += 4096)
    storage_[off] = byte{0};
```

Pages are committed at construction time. The hot path `Alloc()` is a single pointer-chase from the free-list — **no `malloc`, no system call, no page fault**.

### 3. Thread-Isolated Matching via SPSC Ring Buffer

Market data ingestion runs on Core 0. The matching engine runs on Core 1. They communicate through a **wait-free SPSC ring buffer** — no mutex, no futex, no kernel involvement. `head_` and `tail_` counters sit on **separate cache lines** to eliminate producer–consumer false sharing:

```cpp
alignas(64) std::atomic<size_t> head_ {0};
size_t _pad0[7] {};   // fills rest of cache line
alignas(64) std::atomic<size_t> tail_ {0};
```

The matching engine never stalls waiting for network packets. OS-level interrupt jitter on Core 0 does not add latency to the hot path on Core 1.

### 4. Zero-Copy ITCH Ingestion

```cpp
void* raw = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
madvise(raw, file_size, MADV_SEQUENTIAL);  // aggressive readahead
```

The ITCH binary is read directly from the page cache — no `read()` syscall copies bytes into a userspace buffer. `MADV_SEQUENTIAL` tells the kernel to prefetch future pages while the parser processes current ones.

### 5. Kernel Bypass (Extension Path)

For production deployment, replace the ITCH file reader with a **DPDK** or **OpenOnload** integration:

```cpp
// Instead of mmap(file):
//   rte_mbuf* pkt = rte_eth_rx_burst(...);  // DPDK zero-copy from NIC
```

This eliminates the Linux network stack entirely — packets go directly from the NIC DMA buffer to the parser, shaving ~5–10 µs of kernel overhead per packet.

---

## Project Structure

```
chronos/
├── include/
│   ├── memory_arena.hpp   # Pre-faulted slab allocator
│   ├── order.hpp          # Intrusive Order struct (≤64 bytes)
│   ├── price_level.hpp    # O(1) intrusive FIFO queue per price
│   ├── order_book.hpp     # Limit Order Book (bids + asks + id map)
│   ├── spsc_ring.hpp      # Wait-free SPSC ring buffer
│   ├── spsc_logger.hpp    # Async trade CSV logger
│   ├── itch_parser.hpp    # NASDAQ ITCH 5.0 zero-copy parser
│   └── cpu_utils.hpp      # Core pinning, RDTSC, prefetch hints
├── src/
│   ├── order_book.cpp     # LOB matching implementation
│   ├── itch_parser.cpp    # mmap + ITCH message decoder
│   ├── spsc_logger.cpp    # Drain loop + CSV writer
│   └── main.cpp           # CLI: synth + replay modes
├── bench/
│   ├── bench_engine.cpp   # Google Benchmark suite & STL baseline
│   └── (bench_latency)    # Compiles same file for standalone p50/p90 percentiles
├── tools/
│   ├── gen_orders.cpp     # Synthetic CSV order generator
│   └── itch_replay.cpp    # Standalone ITCH replay binary
├── scripts/
│   ├── generate_proof.sh  # Generates apples-to-apples baseline proofs
│   └── gen_sample_itch.py # Generate a test ITCH binary
├── data/                  # Place NASDAQ sample .itch files here
└── CMakeLists.txt
```

---

## Build & Run

### Requirements

| Tool               | Version                                   |
| ------------------ | ----------------------------------------- |
| GCC / Clang        | ≥ 10 (C++20)                              |
| CMake              | ≥ 3.16                                    |
| Linux              | kernel ≥ 4.15 (for `CLOCK_MONOTONIC_RAW`) |
| Google Benchmark   | optional (auto-detected)                  |
| linux-tools (perf) | optional (L1 analysis)                    |

### Quick Start

```bash
# 1. Configure & build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel $(nproc)

# 2. Run synthetic benchmark (5 M orders)
./build/chronos_engine synth 5000000

# 3. Run latency benchmark (p50/p90/p99)
./build/bench_latency

# 4. Generate a test ITCH file and replay it
python3 scripts/gen_sample_itch.py
./build/itch_replay data/sample.itch

# 5. Generate Throughput Baseline Proofs
bash scripts/generate_proof.sh
```

### Using a Real NASDAQ ITCH File

Download a TotalView-ITCH 5.0 sample from:

> https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/

```bash
# Decompress and replay for AAPL
./build/itch_replay data/01302020.NASDAQ_ITCH50 AAPL
```

### Generating a Synthetic Order Dataset

```bash
# 10 million orders with edge cases
./build/gen_orders 10000000 data/orders_10M.csv
```

---

## Performance Results

Benchmarked on an 8-Core 2.1 GHz CPU (Linux), `-O3 -march=native`:

| Metric                    | Result         | Target     |
| ------------------------- | -------------- | ---------- |
| p50 AddOrder (resting)    | ~14 ns         | < 80 ns ✓  |
| p50 Match (crossing fill) | ~40 ns         | < 80 ns ✓  |
| p50 CancelOrder           | ~12 ns         | < 80 ns ✓  |
| Market Sweep (500 levels) | ~14 ns per fill| —          |
| Throughput                | ~71 M orders/s | > 20 M/s ✓ |
| Heap allocs on hot path   | **0**          | 0 ✓        |

> **Note**: Results vary by CPU generation, NUMA topology, and core isolation. Run `run_bench.sh` to reproduce on your hardware.

---

## Profiling for L1 Cache Miss Evidence

```bash
# After building, collect cache statistics
mkdir -p results
perf stat -e cache-references,cache-misses,L1-dcache-load-misses \
    ./build/bench_engine 2>&1 | tee results/perf_report.txt

# Generate a flame graph (requires FlameGraph repo)
perf record -o results/perf.data -g -F 999 ./build/bench_engine
perf script -i results/perf.data | stackcollapse-perf.pl | flamegraph.pl > results/flame.svg
```

The intrusive list design should show **L1-dcache-load-misses < 1%** during the matching loop. If you compare against a version using `std::list<Order*>` (external nodes), cache misses will be 5–10× higher.

---

## Extension: Kernel Bypass with DPDK

```bash
# Install DPDK (Ubuntu)
sudo apt-get install dpdk dpdk-dev

# In CMakeLists.txt, add:
find_package(PkgConfig REQUIRED)
pkg_check_modules(DPDK REQUIRED libdpdk)
target_link_libraries(chronos_engine PRIVATE ${DPDK_LIBRARIES})
```

Replace `ITCHParser::ParseFile()` with a DPDK polling loop on an SR-IOV NIC port. This eliminates the Linux kernel network stack entirely — the CPU reads packets directly from NIC DMA memory.

---

