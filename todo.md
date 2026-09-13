cmake -B build -DRB_BUILD_EXAMPLES=ON -DRB_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Tests still pass
ctest --test-dir build --output-on-failure

# IPC example runs
./build/examples/ipc_reader &
./build/examples/ipc_writer
wait
# Cleanup
rm -f /dev/shm/rb_ipc_demo

---

nm -u build/librb.a | grep -E 'pthread|thrd_'
# (should print nothing)

nm -u build/librb_thread.a | grep pthread
# (should show pthread_create, pthread_join, etc.)

---

cmake -B build -DRB_BUILD_EXAMPLES=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
rm -f /dev/shm/rb_ipc_demo

# Scenario 2: sustained overload with WAIT policy
./build/examples/ipc_slow_reader -d 2000 -v &
./build/examples/ipc_burst_writer -b 256 -q 20 -n 500000 -m wait -v
wait
rm -f /dev/shm/rb_ipc_demo

---

cmake -B build -DRB_BUILD_EXAMPLES=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Notification demo
./build/examples/notify_threaded

# In-process backpressure, both policies
./build/examples/backpressure_inproc
./build/examples/backpressure_inproc -m drop

# GPU (skipped if no CUDA)
./build/examples/gpu_shared

---

1. Graceful shutdown — the biggest gap

PENDING — shutdown protocol has not yet been changed. The producer/consumer termination handshake still needs to be designed and applied consistently across the examples.

2. ABA on the futex word — DONE (bounded backstop)

rb_wait() now uses a bounded futex sleep interval (RB_NOTIFY_WAIT_SLICE_MS, default 1000 ms), rechecks the ring between slices, and uses a monotonic-clock deadline for finite waits. This prevents an indefinite stale sleep if the uint32 head sequence wraps back to the expected value. The futex word remains the uint32 head; a separate 64-bit sequence is therefore not claimed as a kernel-level ABA fix.

3. EINTR handling in rb_wait — DONE

rb_wait() now retries futex waits interrupted by EINTR and preserves the caller's total timeout rather than returning -EINTR. The ring is rechecked after wakeups/interruption.

4. Cross-process notification — PARTIAL

RB_FUTEX_SHARED is now exposed by CMake and switches rb_wait/rb_futex_wake between FUTEX_WAIT(_PRIVATE) and FUTEX_WAKE(_PRIVATE). IPC validation and a dedicated cross-process regression test are still pending. eventfd remains process-local unless its descriptor is explicitly shared by the application.

5. False sharing at the ring/scratchpad boundary — DONE (documented)

The cache-line layout guidance is documented in docs/CACHE_LAYOUT.md. It specifies rb_size(capacity) as the authoritative control-block size and documents cache-line alignment for an adjacent scratchpad. The existing IPC layout rounding remains the required IPC-specific protection.

6. NUMA and affinity hints — DONE (documented)

NUMA/CPU-affinity guidance has been added to docs/DEPENDENCIES.md. It deliberately avoids hard-coded latency numbers and recommends same-socket placement and consumer-node scratchpad allocation for latency-sensitive multi-socket deployments.

7. Batch publish / consume for extreme throughput

The current API does one slot per rb_publish. For very high throughput, the atomic store to head is the bottleneck (~20 ns each). A batch API advances head once for N items:
c

rb_err_t rb_acquire_batch(rb_t *rb, uint32_t n,
                          uint32_t *out_indices, void **out_writable,
                          uint32_t *out_cap);
rb_err_t rb_publish_batch(rb_t *rb, uint32_t n);

The producer writes into N slots, then a single head += n with release ordering publishes all N at once. The consumer drains with rb_consume_batch. Same protocol, N× fewer atomic operations.

This is what the LFQueue benchmark (121 M ops/s → 412 M with batching) is measuring. It's a real 3–4× throughput win at the cost of a more complex API. Whether it's worth it depends on whether the single-item API is your bottleneck. For WS frames, it isn't. For high-frequency packet processing, it is.

8. pkg-config file — DONE

rb.pc.in is now configured by CMake at build time and installed to ${CMAKE_INSTALL_LIBDIR}/pkgconfig as rb.pc. The generated file uses the configured install prefix/libdir/includedir and project version.

9. Fuzzing the state machine — PARTIAL

fuzz/rb_state.c is now integrated as an optional rb_fuzz_state executable through RB_BUILD_FUZZERS. The target requires Clang/libFuzzer and uses address + libFuzzer sanitizers. An actual fuzz run and CI job have not yet been executed/added, so this remains partial.

The harness exercises acquire/publish/abort, consume/release, drain, and state queries while checking the basic count/capacity invariant. It won't find race conditions — that's what TSan does.

10. Signal handling during the wait

PENDING — no regression test has been added yet.

---

include(CheckSymbolExists)

check_symbol_exists(shm_open "sys/mman.h" HAVE_SHM_OPEN_IN_LIBC)

if (NOT HAVE_SHM_OPEN_IN_LIBC)
    set(_rb_ipc_libs rt)
else()
    set(_rb_ipc_libs)
endif()

add_executable(ipc_writer ipc_writer.c)
target_link_libraries(ipc_writer PRIVATE rb ${_rb_ipc_libs})
