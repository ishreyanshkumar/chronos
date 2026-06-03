#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# Chronos :: Benchmark Runner
#
# Builds in Release mode, runs the latency benchmark, and optionally collects
# a Linux perf report for L1 cache-miss analysis.
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
PERF_OUT="$SCRIPT_DIR/perf_report.txt"

# ── Build ─────────────────────────────────────────────────────────────────────
echo "━━━ Building Chronos (Release) ━━━"
cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS="-O3 -march=native" \
    -Wno-dev -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build "$BUILD_DIR" --parallel "$(nproc)"
echo "Build complete."

# ── Latency benchmark ─────────────────────────────────────────────────────────
echo ""
echo "━━━ Running Latency Benchmark ━━━"
"$BUILD_DIR/bench_engine"

# ── Throughput (synthetic, 10M orders) ────────────────────────────────────────
echo ""
echo "━━━ Running Throughput Test (10M orders) ━━━"
"$BUILD_DIR/chronos_engine" synth 10000000

# ── Perf L1 miss analysis (optional – requires linux-perf) ───────────────────
if command -v perf &> /dev/null; then
    echo ""
    echo "━━━ Collecting perf cache stats ━━━"
    perf stat -e cache-references,cache-misses,L1-dcache-loads,L1-dcache-load-misses,\
instructions,cycles \
        "$BUILD_DIR/bench_engine" 2>&1 | tee "$PERF_OUT"
    echo ""
    echo "perf output saved to: $PERF_OUT"
else
    echo ""
    echo "[INFO] 'perf' not found – skipping L1 cache analysis."
    echo "       Install with: sudo apt-get install linux-tools-generic"
fi

echo ""
echo "━━━ Done ━━━"
