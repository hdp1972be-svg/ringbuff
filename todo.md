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

Every example checks a stop / producer_done flag, and every one of them has a subtle race. The consumer reads done=true, breaks, and misses whatever the producer published just before setting it. Or it drains one last time, but the producer published between the drain and the check.

The correct pattern is a three-part condition:
c

for (;;) {
    rb_drain(rb, fn, user);
    if (atomic_load(&producer_done) && rb_is_empty(rb)) break;
    /* else: snapshot, wait, retry */
}

But even that has a window. The producer must:
c

/* publish everything */
atomic_store_release(&producer_done, true);
/* then notify one last time so an asleep consumer re-checks */
rb_signal_notify(&sig, RB_SIG_DATA);

And the consumer must re-check the ring after seeing done, because the producer might have published between the last drain and the store to done. This is the "termination handshake" and it's genuinely tricky.

For the 500-node scenario, this becomes the actor termination problem — every node needs a defined shutdown protocol. Worth designing before you build the bus.
2. ABA on the futex word

STATUS: PARTIAL — bounded wait-slice configuration has been added; the futex still uses the uint32 head word and the monotonic sequence counter is not yet wired into rb_wait.

rb_wait(rb, expected, timeout) uses head as the futex word. head is uint32_t. At 70M ops/s, it wraps in ~61 seconds.

If the consumer snapshots head = V, then drains for 61 seconds (blocked on something, or the system suspends), the producer can wrap all the way around to V. futex_wait(&head, V) sees head == V and sleeps. A finite wait backstop avoids an infinite stale wait.

3. EINTR handling in rb_wait

PENDING — rb_wait still returns EINTR; the retry loop has not yet been implemented.

4. Cross-process notification

PARTIAL — RB_FUTEX_SHARED=0 configuration support has been added to the portability configuration, but rb.c/CMake wiring and IPC validation are still pending.

5. False sharing at the ring/scratchpad boundary — DONE (documented)

The cache-line layout guidance is now documented in docs/CACHE_LAYOUT.md. It specifies rb_size(capacity) as the authoritative control-block size and documents cache-line alignment for an adjacent scratchpad. The existing IPC layout rounding remains the required IPC-specific protection.

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

8. pkg-config file

PARTIAL — rb.pc.in has been added. CMake install/configure wiring is still pending, so the installed package does not yet provide rb.pc.

9. Fuzzing the state machine

PARTIAL — fuzz/rb_state.c has been added as a libFuzzer entry point. CMake integration and an actual fuzz run are still pending.

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
