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

rb_wait(rb, expected, timeout) uses head as the futex word. head is uint32_t. At 70M ops/s, it wraps in ~61 seconds.

If the consumer snapshots head = V, then drains for 61 seconds (blocked on something, or the system suspends), the producer can wrap all the way around to V. futex_wait(&head, V) sees head == V and sleeps. The consumer never wakes.

The timeout saves you. If you call rb_wait(..., -1) (infinite), you have a latent bug. If you call it with a 100 ms timeout, worst case the consumer wakes 100 ms late and re-checks.

Two fixes, either works:

    Always use a finite timeout. Document it. rb_wait(rb, v, 100) is the safe default. Add an assert or a -DRB_WAIT_REQUIRES_TIMEOUT compile guard.

    Use a separate monotonic wake counter. A uint64_t that increments on every rb_signal_notify, not every publish. Wraps in ~8000 years at 70M wakes/s. Costs 4 bytes in the control block and a relaxed atomic increment per notify.

I'd do both — the counter for correctness, and document the timeout as the backstop.
3. EINTR handling in rb_wait

futex_wait returns EINTR when a signal arrives. If rb_wait doesn't loop on EINTR, a stray SIGCHLD or SIGWINCH will spuriously return from the wait. The consumer re-checks the ring (empty), then loops back to rb_wait, so it's functionally correct — just wasteful. Verify the current implementation handles it, and if not, wrap the syscall in a while (rc == -1 && errno == EINTR) loop.

On Zynq this matters more: the PL→ARM IRQ arrives as a signal or an eventfd. If rb_wait doesn't handle EINTR, you'll miss the interrupt-driven path.
4. Cross-process notification

rb_wait uses FUTEX_WAIT_PRIVATE, which is keyed on the process's mm_struct. It works between threads in one process. It does not work between processes sharing MAP_SHARED memory.

For the IPC example, the reader either polls (current design) or uses rb_notify_fd() — an eventfd created by the writer and passed to the reader via fork, SCM_RIGHTS over a Unix socket, or a named pipe. The eventfd approach is what you want for production; the polling approach is fine for a demo.

Alternatively, replace FUTEX_WAIT_PRIVATE with the non-private FUTEX_WAIT. It works on MAP_SHARED memory, at the cost of a shared hash bucket lookup (slower wake path). Worth exposing as a compile-time option: RB_FUTEX_SHARED=1.
5. False sharing at the ring/scratchpad boundary

If the ring control block and the scratchpad live in the same memory region (as in the IPC example), the last field of the ring and the first slot of the scratchpad can share a cache line. Every write to the last ring field invalidates the cache line the producer needs for the first slot. Small, but measurable.

Fix: pad the ring's control block to a full cache line before entries[], and offset the scratchpad by RB_CACHE_LINE from the end of entries[]. Or just check whether rb_size(capacity) is a multiple of RB_CACHE_LINE and document that scratchpads should start at the next cache line.

In ipc_common.h you already round ipc_scratch_offset() up to a cache line, so this is handled for the IPC case. But rb_init accepts arbitrary scratchpad addresses and doesn't validate the offset between ring and scratchpad. Worth a note in the docs, or an alignment check.
6. NUMA and affinity hints

For 500 nodes on a multi-socket machine: the ring's head and tail are on the same cache line (separated, but within the same control block). If the producer thread and consumer thread are on different NUMA nodes, every head write is a cross-node cache-line transfer. That's ~50–100 ns instead of ~20 ns.

The fix is not in the library — it's placement. The scratchpad should be allocated with numa_alloc_onnode matching the consumer's node. The producer thread should be pinned to a core on the same socket. This is documented as operational guidance, not code.

But it's worth a paragraph in docs/DEPENDENCIES.md or docs/TECHNICAL.md: "For multi-socket systems, pin producer and consumer to the same NUMA node and allocate the scratchpad from that node's memory. Cross-node operation costs ~3× the ring's baseline latency."
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

CMake handles the install correctly. But projects not using CMake (Makefiles, Meson, autotools) can't find_package(rb). A .pc file fixes that in 10 lines:
text

prefix=/usr/local
exec_prefix=${prefix}
libdir=${exec_prefix}/lib
includedir=${prefix}/include

Name: rb
Description: Zero-copy SPSC ring buffer with scratchpad
Version: 1.0.0
Libs: -L${libdir} -lrb
Cflags: -I${includedir}

Then non-CMake consumers do pkg-config --cflags --libs rb. Standard convention, low effort.
9. Fuzzing the state machine

The acquire/publish/consume/release protocol has a small state space: 4 operations, strict pairing rules, ~16 states. A libFuzzer target that feeds random operation sequences into the ring and checks invariants (never double-consume, never leak a slot, never crash) would find mis-pairing bugs in 20 minutes of CPU time.

It won't find race conditions — that's what TSan does. But it will find API misuse patterns that unit tests don't cover, and it's cheap insurance against future changes.
10. Signal handling during the wait

Related to #3, but worth separate mention: if a signal arrives while the producer is blocked in a full-ring rb_wait (via the DROP-less WAIT policy), it needs to re-check the ring state and continue waiting. Same for the consumer. The right shape is:
c

while (blocked_condition) {
    int rc = futex_wait(...);
    if (rc == -1 && errno == EINTR) continue;
    /* re-check condition, maybe break */
}

Add a test: install a 10 ms SIGALRM handler, block in rb_wait, verify the wait resumes and the ring still works. That's the kind of bug that shows up in production but never in tests.

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
