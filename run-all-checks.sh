#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 H. De Pauw

# Build and test every variant. Fails fast on the first broken configuration.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"

run_variant() {
    local name="$1"; shift
    local dir="build-$name"
    echo
    echo "════════════════════════════════════════════════════════════"
    echo "  $name"
    echo "════════════════════════════════════════════════════════════"
    rm -rf "$dir"
    cmake -B "$dir" -DRB_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo "$@" >/dev/null
    cmake --build "$dir" -j "$JOBS" >/dev/null
    ctest --test-dir "$dir" --output-on-failure
}

run_variant default
run_variant pointers   -DRB_USE_POINTERS=ON
run_variant cachepad   -DRB_SLOT_CACHELINE_PAD=ON
run_variant tsan \
    -DCMAKE_C_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
run_variant asan \
    -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"

echo
echo "All variants passed."
