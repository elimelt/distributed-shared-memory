#!/bin/bash
# E2E Exhaustive Fuzz Test for DSM Cluster
# Collects performance stats while running random workloads

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_DIR"

# Configuration
NUM_NODES=${NUM_NODES:-3}
PAGES_PER_NODE=${PAGES_PER_NODE:-1024}
BASE_CLIENT_PORT=${BASE_CLIENT_PORT:-9001}
BASE_CLUSTER_PORT=${BASE_CLUSTER_PORT:-10001}
FUZZ_DURATION=${FUZZ_DURATION:-30}
PERF_OUTPUT_DIR=${PERF_OUTPUT_DIR:-"$PROJECT_DIR/perf_results"}
VERBOSE=${VERBOSE:-0}
PIDS=()
TOTAL_PAGES=0

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log() { echo -e "${BLUE}[$(date +%H:%M:%S)]${NC} $*"; }
log_ok() { echo -e "${GREEN}[OK]${NC} $*"; }
log_err() { echo -e "${RED}[ERROR]${NC} $*"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }

cleanup() {
    log "Cleaning up..."
    for pid in "${PIDS[@]}"; do
        kill -9 $pid 2>/dev/null || true
    done
    pkill -9 -f "dsm-client.*localhost" 2>/dev/null || true
    sleep 0.5
}
trap cleanup EXIT

rm -rf "$PERF_OUTPUT_DIR"
mkdir -p "$PERF_OUTPUT_DIR"

# Build if needed
if [[ ! -x ./dsm-server ]] || [[ ! -x ./dsm-client ]]; then
    log "Building DSM binaries..."
    make -j$(nproc) || { log_err "Build failed"; exit 1; }
fi

start_cluster() {
    local num_nodes=$1
    log "Starting $num_nodes-node cluster..."

    PIDS=()
    TOTAL_PAGES=$((num_nodes * PAGES_PER_NODE))

    for ((i=0; i<num_nodes; i++)); do
        local client_port=$((BASE_CLIENT_PORT + i))
        local cluster_port=$((BASE_CLUSTER_PORT + i))
        local range_start=$((i * PAGES_PER_NODE))
        local range_end=$(((i + 1) * PAGES_PER_NODE))

        local cmd="./dsm-server -p $client_port -c $cluster_port -r $range_start:$range_end -n $PAGES_PER_NODE"

        if ((i > 0)); then
            cmd="$cmd -s localhost -S $BASE_CLUSTER_PORT"
        fi

        if ((VERBOSE)); then
            cmd="$cmd -v"
        fi

        $cmd &
        PIDS+=($!)
        log "  Node $i: port=$client_port cluster=$cluster_port range=$range_start-$range_end (PID ${PIDS[$i]})"

        # Small delay between node starts
        sleep 0.5
    done

    # Wait for cluster to stabilize
    sleep 2
    log_ok "Cluster started with $num_nodes nodes, $TOTAL_PAGES total pages"
}

run_fuzz_client() {
    local name=$1
    local node=$2
    local iterations=$3
    local locality=$4
    local write_pct=$5
    local virtual_pages=$6
    local timeout_sec=${7:-30}

    local port=$((BASE_CLIENT_PORT + node))

    timeout $timeout_sec ./dsm-client -H localhost -p $port \
        -N $virtual_pages \
        -n $((virtual_pages / 4)) \
        -i $iterations \
        -l $locality \
        -w $write_pct \
        -b 2>&1 | tee "$PERF_OUTPUT_DIR/${name}.log" || {
        log_warn "Client $name timed out or failed"
        return 1
    }
}

# Test scenarios: node iterations locality write_pct
declare -A SCENARIOS
SCENARIOS["sequential_read"]="0 500 100 0"
SCENARIOS["sequential_write"]="0 500 100 100"
SCENARIOS["random_read"]="0 500 0 0"
SCENARIOS["random_write"]="0 500 0 100"
SCENARIOS["mixed_locality"]="0 500 50 20"
SCENARIOS["high_locality"]="0 500 90 10"
SCENARIOS["cross_node"]="0 500 0 30"

run_scenario() {
    local name=$1
    local params=${SCENARIOS[$name]}
    local node iterations locality write_pct
    read node iterations locality write_pct <<< "$params"

    log "Running scenario: $name (node=$node iter=$iterations loc=$locality% write=$write_pct%)"
    run_fuzz_client "$name" $node $iterations $locality $write_pct $TOTAL_PAGES
}

run_concurrent_clients() {
    local num_clients=$1
    local iterations=$2
    local timeout_sec=${3:-20}
    log "Running $num_clients concurrent clients (timeout=${timeout_sec}s)..."

    local pids=()
    for ((i=0; i<num_clients; i++)); do
        local node=$((i % NUM_NODES))
        local locality=$((RANDOM % 100))
        local write_pct=$((RANDOM % 50))
        run_fuzz_client "concurrent_$i" $node $iterations $locality $write_pct $TOTAL_PAGES $timeout_sec &
        pids+=($!)
    done

    for pid in "${pids[@]}"; do
        wait $pid || log_warn "Client $pid failed"
    done
    log_ok "Concurrent test complete"
}

run_chaos_test() {
    local duration=$1
    log "Running chaos test for ${duration}s (sequential)..."

    local end_time=$((SECONDS + duration))
    local test_num=0

    while ((SECONDS < end_time)); do
        test_num=$((test_num + 1))
        local node=$((RANDOM % NUM_NODES))
        local iterations=$((50 + RANDOM % 150))
        local locality=$((RANDOM % 100))
        local write_pct=$((RANDOM % 100))

        run_fuzz_client "chaos_$test_num" $node $iterations $locality $write_pct $TOTAL_PAGES 3 || true
    done

    log_ok "Chaos test complete: $test_num iterations"
}

run_node_failure_test() {
    log "Running node failure test..."

    # Start background client with timeout
    run_fuzz_client "failure_test" 0 5000 50 20 $TOTAL_PAGES 15 &
    local client_pid=$!

    sleep 2

    # Kill middle node
    if ((NUM_NODES > 1)); then
        local victim=$((NUM_NODES / 2))
        log_warn "Killing node $victim (PID ${PIDS[$victim]})"
        kill -9 ${PIDS[$victim]} 2>/dev/null || true
        sleep 2
    fi

    wait $client_pid || log_warn "Client exited with error (expected during failure)"
    log_ok "Node failure test complete"
}

collect_perf_stats() {
    local name=$1
    shift
    local cmd="$@"

    if command -v perf &>/dev/null; then
        log "Collecting perf stats for: $name"
        timeout 60 perf stat -d -d -d -o "$PERF_OUTPUT_DIR/${name}_perf.txt" -- $cmd 2>&1 || {
            log_warn "Perf collection timed out or failed"
        }
    else
        log_warn "perf not available, running without stats"
        timeout 60 $cmd || log_warn "Command timed out"
    fi
}

generate_report() {
    log "Generating performance report..."

    local report="$PERF_OUTPUT_DIR/report.txt"
    {
        echo "=== DSM Cluster E2E Fuzz Test Report ==="
        echo "Date: $(date)"
        echo "Nodes: $NUM_NODES"
        echo "Pages per node: $PAGES_PER_NODE"
        echo "Total pages: $TOTAL_PAGES"
        echo ""
        echo "=== Test Results ==="

        for log in "$PERF_OUTPUT_DIR"/*.log; do
            [[ -f "$log" ]] || continue
            echo ""
            echo "--- $(basename "$log" .log) ---"
            grep -E "(Cycles|hits|fetches|Evictions|rate)" "$log" || true
        done

        echo ""
        echo "=== Perf Stats Summary ==="
        for perf in "$PERF_OUTPUT_DIR"/*_perf.txt; do
            [[ -f "$perf" ]] || continue
            echo ""
            echo "--- $(basename "$perf" _perf.txt) ---"
            grep -E "(cycles|instructions|cache|branches)" "$perf" | head -10 || true
        done
    } > "$report"

    log_ok "Report saved to $report"
    cat "$report"
}


# Main test runner
main() {
    local mode=${1:-all}

    log "=== DSM Cluster E2E Fuzz Test ==="
    log "Mode: $mode"
    log "Nodes: $NUM_NODES, Pages/node: $PAGES_PER_NODE"

    cleanup
    start_cluster $NUM_NODES

    case $mode in
        quick)
            log "Running quick test suite..."
            run_scenario "high_locality"
            run_scenario "random_read"
            ;;
        scenarios)
            log "Running all scenarios..."
            for scenario in "${!SCENARIOS[@]}"; do
                run_scenario "$scenario"
            done
            ;;
        concurrent)
            log "Running concurrent test..."
            run_concurrent_clients 5 1000 20
            ;;
        chaos)
            log "Running chaos test..."
            run_chaos_test $FUZZ_DURATION
            ;;
        failure)
            log "Running failure test..."
            run_node_failure_test
            ;;
        perf)
            log "Running with perf stats..."
            collect_perf_stats "perf_random" ./dsm-client -H localhost -p $BASE_CLIENT_PORT \
                -N $TOTAL_PAGES -n 256 -i 5000 -l 50 -w 20 -b
            ;;
        all)
            log "Running full test suite..."

            # Basic scenarios
            for scenario in "${!SCENARIOS[@]}"; do
                run_scenario "$scenario"
            done

            # Concurrent access
            run_concurrent_clients 3 500 15

            # Chaos testing (short)
            run_chaos_test 5

            # Node failure test
            cleanup
            start_cluster $NUM_NODES
            run_node_failure_test

            # Perf collection
            cleanup
            start_cluster $NUM_NODES
            collect_perf_stats "final_perf" ./dsm-client -H localhost -p $BASE_CLIENT_PORT \
                -N $TOTAL_PAGES -n 256 -i 1000 -l 70 -w 15 -b
            ;;
        *)
            echo "Usage: $0 [quick|scenarios|concurrent|chaos|failure|perf|all]"
            exit 1
            ;;
    esac

    generate_report
    log_ok "All tests complete!"
}

main "$@"

