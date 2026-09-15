#!/usr/bin/env bash
set -euo pipefail

MEMREF=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        -m|--memcpy) MEMREF=1; shift ;;
        *) break ;;
    esac
done
DURATION="${1:-${BENCH_SECONDS:-3}}"
BIN_DIR="${BIN_DIR:-$(dirname "$0")/../build-errors/examples}"
SHM="/dev/shm/rb_ipc_bench"
SIZES="4 16 256 1024 2048 4096"

for bin in "$BIN_DIR/ipc_bench_writer" "$BIN_DIR/ipc_bench_reader"; do
    if [ ! -x "$bin" ]; then
        echo "$0: not found or not executable: $bin" >&2
        exit 1
    fi
done
if [ "$MEMREF" -eq 1 ] && [ ! -x "$BIN_DIR/ipc_bench_memcpy" ]; then
    echo "$0: not found or not executable: $BIN_DIR/ipc_bench_memcpy" >&2
    exit 1
fi

field() {
    sed -n "s|$2|\1|p" "$1" | head -n1 || true
}

printf "%-6s  %12s  %10s  %12s  %10s  %8s  %s\n" \
    size w_msg/s w_MB/s r_msg/s r_MB/s gaps latency

trap 'rm -f "$tmpw" "$tmpr"' EXIT
tmpw=$(mktemp)
tmpr=$(mktemp)

for sz in $SIZES; do
    rm -f "$SHM"

    "$BIN_DIR/ipc_bench_writer" -t "$DURATION" -s "$sz" > "$tmpw" 2>&1 &
    wpid=$!
    "$BIN_DIR/ipc_bench_reader" -t "$DURATION" -s "$sz" > "$tmpr" 2>&1
    wait "$wpid" 2>/dev/null || true

    wmsg=$(field "$tmpw" '.*measured [0-9.]* s, \([0-9]*\) msg/s.*')
    wmb=$(field "$tmpw" '.* msg/s, \([0-9.]*\) MB/s.*')
    rmsg=$(field "$tmpr" '.*measured [0-9.]* s, \([0-9]*\) msg/s.*')
    rmb=$(field "$tmpr" '.* msg/s, \([0-9.]*\) MB/s.*')
    gaps=$(field "$tmpr" '.*, \([0-9]*\) gaps,.*')
    lat=$(field "$tmpr" '.*latency \(.*\) ns.*')

    printf "%-6s  %12s  %10s  %12s  %10s  %8s  %s\n" \
        "$sz" "${wmsg:--}" "${wmb:--}" "${rmsg:--}" "${rmb:--}" "${gaps:--}" "${lat:-n/a}"
done

if [ "$MEMREF" -eq 1 ]; then
    printf "\n%-6s  %12s  %10s\n" size c_msg/s c_MB/s
    for sz in $SIZES; do
        "$BIN_DIR/ipc_bench_memcpy" -t "$DURATION" -s "$sz" > "$tmpr" 2>&1
        cmsg=$(field "$tmpr" '.*measured [0-9.]* s, \([0-9]*\) msg/s.*')
        cmb=$(field "$tmpr" '.* msg/s, \([0-9.]*\) MB/s.*')
        printf "%-6s  %12s  %10s\n" "$sz" "${cmsg:--}" "${cmb:--}"
    done
fi

rm -f "$SHM"
