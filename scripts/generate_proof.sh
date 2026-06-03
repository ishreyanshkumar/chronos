#!/bin/bash
mkdir -p results

echo "================================================================" > results/throughput_proof.txt
echo "         CHRONOS — THROUGHPUT PROOF" >> results/throughput_proof.txt
echo "         Synthetic Benchmark @ 5,000,000 Orders" >> results/throughput_proof.txt
echo "================================================================" >> results/throughput_proof.txt
echo "" >> results/throughput_proof.txt

echo "----------------------------------------------------------------" >> results/throughput_proof.txt
echo "TEST 1: Core Matching Speed  (synth_fast)" >> results/throughput_proof.txt
echo "        Logging DISABLED — pure engine throughput, no I/O overhead" >> results/throughput_proof.txt
echo "----------------------------------------------------------------" >> results/throughput_proof.txt
./build/chronos_engine synth_fast 5000000 >> results/throughput_proof.txt
echo "" >> results/throughput_proof.txt

echo "----------------------------------------------------------------" >> results/throughput_proof.txt
echo "TEST 2: End-to-End Pipeline  (synth)" >> results/throughput_proof.txt
echo "        Logging ENABLED — matching + concurrent SPSC trade logger" >> results/throughput_proof.txt
echo "----------------------------------------------------------------" >> results/throughput_proof.txt
./build/chronos_engine synth 5000000 >> results/throughput_proof.txt

echo "Generating benchmark JSON..."
./build/bench_engine --benchmark_format=json > results/benchmark_results.json

echo "Copying flamegraph..."
if [ -f results/flame.svg ]; then
    cp results/flame.svg results/flamegraph_custom_engine.svg
fi

echo "Done!"
