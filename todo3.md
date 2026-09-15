Two patches. Both go into existing files. No API changes.
Patch 1 — rb_port.h additions

Add these near the bottom, before the #endif:
c

/* ---------------- Memory operations ----------------
 * RB_MEMCPY / RB_MEMSET default to the C library versions. Override at
 * build time to substitute a platform-tuned implementation:
 *
 *     -DRB_MEMCPY=arm_fast_memcpy -DRB_MEMSET=arm_fast_memset
 *
 * The library calls these for the 4-byte slot header only. The payload
 * copy is the caller's responsibility and is not routed through here.
 */
#ifndef RB_MEMCPY
#  define RB_MEMCPY memcpy
#endif
#ifndef RB_MEMSET
#  define RB_MEMSET memset
#endif

/* ---------------- Prefetch ----------------
 * RB_PREFETCH_R(p) hints that *p will be read soon.
 * RB_PREFETCH_W(p) hints that *p will be written soon.
 *
 * On x86 and ARM these compile to a single instruction. On targets
 * without a prefetch instruction they compile to nothing. The hint is
 * advisory; correctness never depends on it.
 */
#if defined(__GNUC__) || defined(__clang__)
#  define RB_PREFETCH_R(p) __builtin_prefetch((const void *)(p), 0, 3)
#  define RB_PREFETCH_W(p) __builtin_prefetch((const void *)(p), 1, 3)
#else
#  define RB_PREFETCH_R(p) ((void)0)
#  define RB_PREFETCH_W(p) ((void)0)
#endif

rb_port.h already includes <stddef.h> and <stdint.h>. Add <string.h> for memcpy/memset to be declared:
c

#include <string.h>

Now rb.c can drop its own #include <string.h> — or keep it, harmless.
Patch 2 — rb.c changes
2a. Replace memcpy / memset calls

There are two memcpy calls (header write in rb_publish_ex, header read in rb_consume) and two memset calls (rb_init's control-block clear, rb_stats_reset).

In rb_publish_ex, find:
c

memcpy(slot, &hdr, sizeof hdr);

Replace with:
c

RB_MEMCPY(slot, &hdr, sizeof hdr);

In rb_consume, find:
c

memcpy(&hdr, slot, sizeof hdr);

Replace with:
c

RB_MEMCPY(&hdr, slot, sizeof hdr);

In rb_init, find:
c

memset(rb, 0, offsetof(rb_t, entries));

Replace with:
c

RB_MEMSET(rb, 0, offsetof(rb_t, entries));

In rb_stats_reset, find:
c

memset(&rb->stats, 0, sizeof rb->stats);

Replace with:
c

RB_MEMSET(&rb->stats, 0, sizeof rb->stats);

Any remaining memset(&rb->stats, 0, ...) in rb_init — same treatment.
2b. Prefetch in rb_acquire

The function ends with filling the out-parameters. Insert the prefetch just before the return:
c

    if (out_slot_index) *out_slot_index = slot_index;
    if (out_writable) {
        uint8_t *slot = rb->scratch + (size_t)slot_index * rb->slot_stride;
        void *w = slot + RB_SLOT_HDR_SIZE;
        *out_writable = w;
        /* The caller will write into this line next. Start the fetch now,
           while it does whatever prep work precedes its memcpy. */
        RB_PREFETCH_W(w);
    }
    if (out_cap) *out_cap = cap;
    return RB_OK;

Note the small restructure: w is computed once, used for both the assignment and the prefetch. If out_writable is NULL (rare), no prefetch is issued — harmless.
2c. Prefetch in rb_consume

The function fills *out_obj with the payload pointer. Insert the prefetch right before the return:
c

    if (out_slot_index) *out_slot_index = slot_index;
    if (out_obj) {
        const void *obj = slot + RB_SLOT_HDR_SIZE;
        *out_obj = obj;
        /* The caller will read from this line next. */
        RB_PREFETCH_R(obj);
    }
    if (out_len)        *out_len        = len;
    if (out_truncated)  *out_truncated  = truncated;
    return RB_OK;

Same restructure for the same reason.
Patch 3 — rb.c header comment (optional)

Add one line to the memory-ordering section of the file header:
text

 * Prefetch hints (RB_PREFETCH_R/W) are issued in rb_acquire and
 * rb_consume. They are advisory only; correctness does not depend on
 * them, and they compile to nothing on targets without a prefetch
 * instruction.

The -DRB_MEMCPY usage

Once the hook is in place, a consumer can override at build time:
bash

cmake -B build -DRB_MEMCPY=my_fast_memcpy -DRB_MEMSET=my_fast_memset

Or on the command line if not using CMake:
bash

cc -DRB_MEMCPY=arm_fast_memcpy ...

The library's own header copies are 4 bytes, so this only matters for a target whose libc memcpy is pathologically slow (some newlib builds). On x86-64 and Android it changes nothing — glibc and Bionic are already optimal for 4-byte copies, and the compiler inlines them.
Verification

After applying both patches:
bash

cmake --build build -j
ctest --test-dir build --output-on-failure

All three tests should pass identically. The prefetch and memcpy substitution are correctness-neutral.

To confirm the prefetch is actually emitted:
bash

objdump -d build-default/CMakeFiles/rb.dir/src/rb.c.o | grep -E 'prefetch|prefetcht0'

You should see prefetcht0 on x86-64 for the write prefetch (_MM_HINT_T0 on write compiles to prefetcht0 or prefetchw depending on CPU target). If you see nothing, the compiler decided the prefetch wasn't worth it — possible if it can already prove the line is hot from context. That's fine; the hint is advisory.
What to expect

For the WS pipeline with 2 KB frames:

    Prefetch in rb_acquire: the producer typically does work between rb_acquire and the memcpy — reading from the socket, parsing a frame header, deciding a route. That's 200–2000 ns of work. Prefetching at rb_acquire time means the slot's first cache line is already in L1 by the time memcpy runs. Expect 5–15% on the producer's per-item cost, depending on how long the gap is.

    Prefetch in rb_consume: the consumer usually inspects the payload immediately. There's no gap to hide the miss behind. The prefetch here helps only if the consumer does bookkeeping before touching the payload — updating stats, calling a threshold callback, logging. Expect 0–5%, workload-dependent.

    RB_MEMCPY hook: zero benefit on glibc/Bionic, real benefit on toolchains with a naive newlib. Free to add, no cost when unused.
