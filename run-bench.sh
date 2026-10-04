#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JOBS="${JOBS:-$(nproc)}"
ITERS="${RB_BENCH_ITERS:-2000000}"
CC="${CC:-gcc-12}"
export CC

echo "============================================================"
echo " ringbuff benchmark matrix"
echo "============================================================"
echo "root       : $ROOT"
echo "jobs       : $JOBS"
echo "iterations : $ITERS"
echo "compiler   : $CC"
echo "date       : $(date)"
echo

# Run as root: on bare-metal systems this benchmark temporarily changes the
# CPU frequency policy and disables Intel Turbo, then restores the original
# settings on exit. Virtualized runners may expose neither interface; in that
# case the benchmark continues without changing CPU policy.
if [[ "${EUID}" -ne 0 ]]; then
    echo "error: run this script as root (e.g. sudo ./run-bench.sh)" >&2
    exit 1
fi

if ! command -v perf >/dev/null 2>&1; then
    echo "error: perf is required for per-mode hardware-counter measurements" >&2
    exit 1
fi

CPU_GOVERNOR="performance"
INTEL_PSTATE_NO_TURBO="/sys/devices/system/cpu/intel_pstate/no_turbo"

GOVERNOR_CONTROL=0
ORIG_GOVERNOR=""
if [[ -r "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor" && -w "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor" ]]; then
    ORIG_GOVERNOR="$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
    GOVERNOR_CONTROL=1
fi

ORIG_NO_TURBO=""
TURBO_CONTROL=0
if [[ -r "$INTEL_PSTATE_NO_TURBO" && -w "$INTEL_PSTATE_NO_TURBO" ]]; then
    ORIG_NO_TURBO="$(cat "$INTEL_PSTATE_NO_TURBO")"
    TURBO_CONTROL=1
fi

restore_cpu_policy() {
    echo
    echo "restoring CPU policy..."
    if [[ "$TURBO_CONTROL" -eq 1 ]]; then
        echo "$ORIG_NO_TURBO" > "$INTEL_PSTATE_NO_TURBO"
    fi
    if [[ "$GOVERNOR_CONTROL" -eq 1 ]]; then
        for governor in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
            [[ -w "$governor" ]] && echo "$ORIG_GOVERNOR" > "$governor" || true
        done
    fi
}
trap restore_cpu_policy EXIT INT TERM

if [[ "$GOVERNOR_CONTROL" -eq 1 ]]; then
    echo "CPU policy: $ORIG_GOVERNOR -> $CPU_GOVERNOR"
    for governor in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        [[ -w "$governor" ]] && echo "$CPU_GOVERNOR" > "$governor"
    done
    GOVERNOR_STATUS="$CPU_GOVERNOR"
else
    echo "CPU governor: control unavailable; continuing without changing it"
    GOVERNOR_STATUS="unavailable"
fi

if [[ "$TURBO_CONTROL" -eq 1 ]]; then
    echo 1 > "$INTEL_PSTATE_NO_TURBO"
    echo "Intel Turbo: disabled"
    TURBO_STATUS="disabled"
else
    echo "Intel Turbo: control unavailable; continuing without changing it"
    TURBO_STATUS="unavailable"
fi

cd "$ROOT"

MODES=(0 1 2 3)
RESULTS_DIR="$ROOT/bench-results"
RESULTS_MD="$RESULTS_DIR/perf-modes.md"
mkdir -p "$RESULTS_DIR"

cat > "$RESULTS_MD" <<EOF
# Ringbuff benchmark + perf results

- Date: $(date -Is)
- Host: $(hostname)
- Iterations: $ITERS
- CPU governor: $GOVERNOR_STATUS
- Intel Turbo control: $TURBO_STATUS
- Modes: 0, 1, 2, 3

EOF

for mode in "${MODES[@]}"; do
    BUILD="$ROOT/build-bench-mode${mode}"

    # Always configure from a clean build tree: no CMake/compiler cache.
    rm -rf "$BUILD"

    echo
    echo "============================================================"
    echo " MODE $mode"
    echo " build: $BUILD"
    echo "============================================================"

    cmake -S "$ROOT" -B "$BUILD" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER="$CC" \
        -DCMAKE_C_FLAGS="-O2" \
        -DRB_BUILD_BENCH=ON \
        -DRB_DIRECT_SLOT_MAP="$mode" \
        -DRB_BUILD_TESTS=OFF \
        -DRB_BUILD_EXAMPLES=OFF \
        -DRB_BUILD_FUZZERS=OFF

    cmake --build "$BUILD" --parallel "$JOBS"

    echo
    echo "--- running mode $mode ---"
    echo

    BENCH_OUT="$RESULTS_DIR/mode${mode}-bench.txt"
    PERF_OUT="$RESULTS_DIR/mode${mode}-perf.txt"
    RB_BENCH_ITERS="$ITERS" "$BUILD/bench/bench_rb" > "$BENCH_OUT"
    cat "$BENCH_OUT"

    perf stat --no-big-num \
        -e cycles,instructions,branches,branch-misses,L1-dcache-loads,L1-dcache-load-misses,LLC-loads,LLC-load-misses \
        -- env RB_BENCH_ITERS="$ITERS" "$BUILD/bench/bench_rb" > /dev/null 2> "$PERF_OUT"

    {
        echo "## Mode $mode"
        echo
        echo "### Benchmark"
        echo
        echo "\`\`\`text"
        cat "$BENCH_OUT"
        echo "\`\`\`"
        echo
        echo "### perf stat"
        echo
        echo "Command: perf stat --no-big-num -e cycles,instructions,branches,branch-misses,L1-dcache-loads,L1-dcache-load-misses,LLC-loads,LLC-load-misses -- bench/bench_rb"
        echo
        echo "\`\`\`text"
        cat "$PERF_OUT"
        echo "\`\`\`"
        echo
    } >> "$RESULTS_MD"
done

echo
echo "============================================================"
echo " benchmark matrix complete"
echo "============================================================"
