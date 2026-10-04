#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JOBS="${JOBS:-$(nproc)}"
ITERS="${RB_BENCH_ITERS:-2000000}"

echo "============================================================"
echo " ringbuff benchmark matrix"
echo "============================================================"
echo "root       : $ROOT"
echo "jobs       : $JOBS"
echo "iterations : $ITERS"
echo "date       : $(date)"
echo

# Run as root: this benchmark temporarily changes the CPU frequency policy
# and disables Intel Turbo, then restores the original settings on exit.
if [[ "${EUID}" -ne 0 ]]; then
    echo "error: run this script as root (e.g. sudo ./run-bench.sh)" >&2
    exit 1
fi

CPU_GOVERNOR="performance"
INTEL_PSTATE_NO_TURBO="/sys/devices/system/cpu/intel_pstate/no_turbo"

ORIG_GOVERNOR="$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
ORIG_NO_TURBO=""
if [[ -f "$INTEL_PSTATE_NO_TURBO" ]]; then
    ORIG_NO_TURBO="$(cat "$INTEL_PSTATE_NO_TURBO")"
fi

restore_cpu_policy() {
    echo
    echo "restoring CPU policy..."
    if [[ -n "$ORIG_NO_TURBO" && -w "$INTEL_PSTATE_NO_TURBO" ]]; then
        echo "$ORIG_NO_TURBO" > "$INTEL_PSTATE_NO_TURBO"
    fi
    for governor in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        [[ -w "$governor" ]] && echo "$ORIG_GOVERNOR" > "$governor" || true
    done
}
trap restore_cpu_policy EXIT INT TERM

echo "CPU policy: $ORIG_GOVERNOR -> $CPU_GOVERNOR"
for governor in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    [[ -w "$governor" ]] && echo "$CPU_GOVERNOR" > "$governor"
done

if [[ -w "$INTEL_PSTATE_NO_TURBO" ]]; then
    echo 1 > "$INTEL_PSTATE_NO_TURBO"
    echo "Intel Turbo: disabled"
else
    echo "Intel Turbo: control unavailable; continuing without changing it"
fi

cd "$ROOT"

MODES=(0 1 2)

for mode in "${MODES[@]}"; do
    BUILD="$ROOT/build-bench-mode${mode}"

    echo
    echo "============================================================"
    echo " MODE $mode"
    echo " build: $BUILD"
    echo "============================================================"

    cmake -S "$ROOT" -B "$BUILD" \
        -DCMAKE_BUILD_TYPE=Release \
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

    RB_BENCH_ITERS="$ITERS" "$BUILD/bench/bench_rb"
done

echo
echo "============================================================"
echo " benchmark matrix complete"
echo "============================================================"
