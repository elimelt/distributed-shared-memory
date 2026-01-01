#!/bin/bash
# Run comprehensive matrix multiplication benchmarks
# Compares single-server vs cluster, and large vs small cache scenarios
# Usage: ./run_full_bench.sh

set -e

cleanup() {
    pkill -9 dsm-server 2>/dev/null || true
}
trap cleanup EXIT

echo "╔══════════════════════════════════════════════════════════════════════╗"
echo "║     Comprehensive DSM Matrix Multiplication Benchmark Suite          ║"
echo "╚══════════════════════════════════════════════════════════════════════╝"
echo ""

cleanup
sleep 1

# Test 1: Single server, large cache (best case)
echo "═══════════════════════════════════════════════════════════════════════"
echo "TEST 1: Single Server, Large Cache (Best Case)"
echo "═══════════════════════════════════════════════════════════════════════"
./dsm-server -p 9000 -n 8192 &
SPID=$!
sleep 1
LD_LIBRARY_PATH=. ./examples/cluster_bench 127.0.0.1 9000 256
kill $SPID 2>/dev/null; wait $SPID 2>/dev/null
echo ""

# Test 2: Single server, small cache (eviction pressure)
echo "═══════════════════════════════════════════════════════════════════════"
echo "TEST 2: Single Server, Small Cache (Eviction Pressure)"
echo "═══════════════════════════════════════════════════════════════════════"
./dsm-server -p 9000 -n 8192 &
SPID=$!
sleep 1
DSM_SMALL_CACHE=1 LD_LIBRARY_PATH=. ./examples/cluster_bench 127.0.0.1 9000 256
kill $SPID 2>/dev/null; wait $SPID 2>/dev/null
echo ""

# Test 3: 2-node cluster, large cache
echo "═══════════════════════════════════════════════════════════════════════"
echo "TEST 3: 2-Node Cluster, Large Cache (Distributed)"
echo "═══════════════════════════════════════════════════════════════════════"
export DSM_PAGE_RANGE_START=0
export DSM_PAGE_RANGE_END=4096
export DSM_CLUSTER_PORT=10000
./dsm-server -p 9000 -n 8192 &
S1PID=$!
unset DSM_PAGE_RANGE_START DSM_PAGE_RANGE_END DSM_CLUSTER_PORT

sleep 2

export DSM_PAGE_RANGE_START=4096
export DSM_PAGE_RANGE_END=8192
export DSM_CLUSTER_PORT=10001
export DSM_SEED_ADDR=127.0.0.1
export DSM_SEED_PORT=10000
./dsm-server -p 9001 -n 8192 &
S2PID=$!
unset DSM_PAGE_RANGE_START DSM_PAGE_RANGE_END DSM_CLUSTER_PORT DSM_SEED_ADDR DSM_SEED_PORT

sleep 5

LD_LIBRARY_PATH=. ./examples/cluster_bench 127.0.0.1 9000 256
kill $S1PID $S2PID 2>/dev/null; wait $S1PID $S2PID 2>/dev/null
echo ""

# Test 4: 2-node cluster, small cache (worst case)
echo "═══════════════════════════════════════════════════════════════════════"
echo "TEST 4: 2-Node Cluster, Small Cache (Worst Case)"
echo "═══════════════════════════════════════════════════════════════════════"
export DSM_PAGE_RANGE_START=0
export DSM_PAGE_RANGE_END=4096
export DSM_CLUSTER_PORT=10000
./dsm-server -p 9000 -n 8192 &
S1PID=$!
unset DSM_PAGE_RANGE_START DSM_PAGE_RANGE_END DSM_CLUSTER_PORT

sleep 2

export DSM_PAGE_RANGE_START=4096
export DSM_PAGE_RANGE_END=8192
export DSM_CLUSTER_PORT=10001
export DSM_SEED_ADDR=127.0.0.1
export DSM_SEED_PORT=10000
./dsm-server -p 9001 -n 8192 &
S2PID=$!
unset DSM_PAGE_RANGE_START DSM_PAGE_RANGE_END DSM_CLUSTER_PORT DSM_SEED_ADDR DSM_SEED_PORT

sleep 5

DSM_SMALL_CACHE=1 LD_LIBRARY_PATH=. ./examples/cluster_bench 127.0.0.1 9000 256
kill $S1PID $S2PID 2>/dev/null; wait $S1PID $S2PID 2>/dev/null
echo ""

echo "═══════════════════════════════════════════════════════════════════════"
echo "BENCHMARK COMPLETE"
echo "═══════════════════════════════════════════════════════════════════════"
