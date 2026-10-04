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
