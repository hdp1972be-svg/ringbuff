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

1. Graceful shutdown — DONE (application-level protocol)

The IPC writer now runs for a configured duration (RB_RUN_SECONDS, default 10 s) instead of a fixed message count and responds to SIGINT by leaving its publish loop cleanly. The reader likewise handles SIGINT and exits through its normal cleanup path. This keeps shutdown out of the hot path and makes sustained runs representative of the surrounding system load. A higher-level application that needs an explicit producer/consumer EOF state can layer that state beside the ring; the ring itself deliberately does not infer producer lifetime from queue state.

2. ABA on the futex word — DONE (bounded backstop)

rb_wait() now uses a bounded futex sleep interval (RB_NOTIFY_WAIT_SLICE_MS, default 100 ms), rechecks the ring between slices, and uses a monotonic-clock deadline for finite waits. This prevents an indefinite stale sleep if the uint32 head sequence wraps back to the expected value. The futex word remains the uint32 head; a separate 64-bit sequence is therefore not claimed as a kernel-level ABA fix.

3. EINTR handling in rb_wait — DONE

rb_wait() now retries futex waits interrupted by EINTR and preserves the caller's total timeout rather than returning -EINTR. The ring is rechecked after wakeups/interruption.

4. Cross-process notification — PARTIAL

RB_FUTEX_SHARED is now exposed by CMake and switches rb_wait/rb_futex_wake between FUTEX_WAIT(_PRIVATE) and FUTEX_WAKE(_PRIVATE). The existing IPC reader now exercises rb_wait() against a MAP_SHARED ring when built with RB_FUTEX_SHARED=ON. The sustained cross-process workflow is supplied separately as .github/workflows/ipc-sustained.yml and records writer/reader logs as an artifact; it still needs to be uploaded and run in GitHub Actions. eventfd remains process-local unless its descriptor is explicitly shared by the application.

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

9. Fuzzing the state machine — DONE (integrated + smoke test)

fuzz/rb_state.c is integrated as an optional rb_fuzz_state executable through RB_BUILD_FUZZERS. The target requires Clang/libFuzzer, instruments both the harness and core rb library with AddressSanitizer + UndefinedBehaviorSanitizer, and registers an rb_fuzz_smoke CTest that runs 1000 fuzzing iterations with inputs capped at 128 bytes.

The harness exercises acquire/publish/abort, consume/release, drain callbacks, and state queries while maintaining a model count and checking it against rb_count(). It won't find race conditions — that's what TSan does. The smoke test is defined but has not been executed in this environment.

10. Signal handling during the wait — DONE

Added tests/test_rb_signal.c and the rb_signal CTest. The regression targets a real futex waiter with SIGUSR1 via pthread_kill(), verifies the signal is observed while the waiter remains blocked, then publishes data and verifies rb_wait() resumes normally instead of leaking EINTR. This complements the existing EINTR retry logic.

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
