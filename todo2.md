Two modes of operation

The ring has one job: tell the producer "this slot is free" and tell the consumer "this slot is ready." Both modes do exactly that. They differ in where that state lives.
Mode 0 — shared counters (current design)

The state lives in the control block, separate from the payload:
text

        control block                    scratchpad
   ┌───────────────────────┐        ┌──────────────────────┐
   │ head  (producer writes)│        │ slot 0: [hdr][data] │
   │ tail  (consumer writes)│        │ slot 1: [hdr][data] │
   │ cached_*              │        │ slot 2: [hdr][data] │
   │ entries[]             │        │ ...                  │
   └───────────────────────┘        └──────────────────────┘
        ▲                                        ▲
        │ two lines ping-pong                    │ one line per access
        │ between cores                          │

Producer checks head - tail < limit. Consumer checks head - tail > 0. Each check reads the other side's line.

Cache lines per operation: 2 — the counter line plus the payload line.
Mode 1 — per-slot lap counters

The state lives on the payload's own cache line:
text

   scratchpad
   ┌──────────────────────────────────┐
   │ slot 0: [seq][pad][data]         │  ← 64 B line
   ├──────────────────────────────────┤
   │ slot 1: [seq][pad][data]         │  ← 64 B line
   ├──────────────────────────────────┤
   │ slot 2: [seq][pad][data]         │  ← 64 B line
   ├──────────────────────────────────┤
   │ ...                              │
   └──────────────────────────────────┘

   control block
   ┌───────────────────────┐
   │ producer_pos (private)│
   │ consumer_pos (private)│
   │ notify_seq (atomic)   │  ← only for the futex
   │ config, callbacks     │
   └───────────────────────┘

Each slot carries its own sequence number on the same cache line as its payload. Producer and consumer each keep a private position counter that only they touch. The only communication is through the slots themselves.

Cache lines per operation: 1 — the payload line carries the readiness signal.
The sequence number protocol

This is the Vyukov sequence scheme, simplified for SPSC. Positions are monotonically increasing uint32_t values. Slot i = pos % N is used for absolute position pos.

Initial state: slot[i].seq = i for i = 0 .. N-1.

Producer at position p:
c

i = p & mask;                    // slot index
expected = p;                    // seq value that means "free"
if (LOAD_ACQ(&slot[i].seq) != expected) return RB_ERR_FULL;
/* write payload into slot[i] */
STORE_REL(&slot[i].seq, p + 1);  // publish: 1 = "written"
producer_pos = p + 1;

Consumer at position p:
c

i = p & mask;
expected = p + 1;                // seq value that means "ready"
if (LOAD_ACQ(&slot[i].seq) != expected) return RB_ERR_EMPTY;
/* read payload from slot[i] */
STORE_REL(&slot[i].seq, p + N);  // release: N later = "free again"
consumer_pos = p + 1;

The sequence number advances by N each time a slot is used. After the consumer releases slot i at position p, its seq becomes p + N. When the producer reaches position p + N, it expects seq == p + N and finds it. The cycle closes.

Critical property: neither side ever reads the other's position. The producer only reads its own slot's seq. The consumer only reads its own slot's seq. There is no shared counter line to ping-pong.
What this changes in the code
Slot layout
c

#if RB_PER_SLOT_LAP
struct slot_header {
    rb_atomic_u32 seq;      /* 4 B on the payload's line */
    uint32_t      _pad;     /* round to 8 for alignment */
    /* payload starts at slot + RB_SLOT_HDR_SIZE */
};
#define RB_SLOT_HDR_SIZE 8u
#else
#define RB_SLOT_HDR_SIZE 4u
#endif

Control block
c

struct rb_s {
#if RB_PER_SLOT_LAP
    uint32_t producer_pos;      /* private to producer thread */
    rb_atomic_u32 notify_seq;   /* for futex wake */
    uint32_t consumer_pos;      /* private to consumer thread */
#else
    rb_atomic_u32 head;
    uint32_t cached_tail;
    /* ... current fields ... */
#endif
    /* everything else unchanged */
};

Publish
c

rb_err_t rb_publish_ex(rb_t *rb, uint32_t slot_index, uint32_t len, bool trunc) {
#if RB_PER_SLOT_LAP
    uint32_t pos = rb->producer_pos;
    /* write header + payload into slot[pos & mask] (already done by caller
       via the writable pointer returned from rb_acquire) */
    rb_atomic_u32 *seq = (rb_atomic_u32 *)
        (rb->scratch + (size_t)(pos & rb->mask) * rb->slot_stride);
    RB_ATOMIC_STORE_REL(seq, pos + 1u);
    rb->producer_pos = pos + 1u;
    /* notify path (futex / eventfd) uses notify_seq */
    RB_ATOMIC_STORE_REL(&rb->notify_seq, pos + 1u);
    /* callbacks, stats, latch logic unchanged */
#else
    /* current implementation */
#endif
}

Acquire
c

rb_err_t rb_acquire(rb_t *rb, uint32_t wanted, uint32_t *out_idx,
                    void **out_w, uint32_t *out_cap) {
#if RB_PER_SLOT_LAP
    uint32_t pos = rb->producer_pos;
    uint32_t i = pos & rb->mask;
    rb_atomic_u32 *seq = (rb_atomic_u32 *)
        (rb->scratch + (size_t)i * rb->slot_stride);
    if (RB_ATOMIC_LOAD_ACQ(seq) != pos) {
        /* ring full (consumer hasn't caught up) */
        return RB_ERR_FULL;
    }
    *out_idx = i;
    *out_w = rb->scratch + (size_t)i * rb->slot_stride + RB_SLOT_HDR_SIZE;
    *out_cap = rb->slot_size - RB_SLOT_HDR_SIZE;
#else
    /* current implementation */
#endif
    return RB_OK;
}

Consume
c

rb_err_t rb_consume(rb_t *rb, uint32_t *out_idx, const void **out_obj,
                    uint32_t *out_len, bool *out_trunc) {
#if RB_PER_SLOT_LAP
    uint32_t pos = rb->consumer_pos;
    uint32_t i = pos & rb->mask;
    const uint8_t *slot = rb->scratch + (size_t)i * rb->slot_stride;
    rb_atomic_u32 *seq = (rb_atomic_u32 *)slot;
    if (RB_ATOMIC_LOAD_ACQ(seq) != pos + 1u) {
        return RB_ERR_EMPTY;
    }
    /* read header (now the length is in the payload area) */
    /* ... */
#else
    /* current implementation */
#endif
    return RB_OK;
}

Release
c

rb_err_t rb_release(rb_t *rb, uint32_t slot_index) {
#if RB_PER_SLOT_LAP
    uint32_t pos = rb->consumer_pos;
    rb_atomic_u32 *seq = (rb_atomic_u32 *)
        (rb->scratch + (size_t)slot_index * rb->slot_stride);
    RB_ATOMIC_STORE_REL(seq, pos + rb->slots);
    rb->consumer_pos = pos + 1u;
    /* low-water callbacks use the local consumer_pos vs rb->limit */
#else
    /* current implementation */
#endif
    return RB_OK;
}

What breaks and what to do about it
rb_count()

In mode 1, there's no shared counter. Options:

A. Side counter, updated on batch boundaries. The producer writes head_snapshot = producer_pos with relaxed ordering every N publishes or on every rb_signal_notify. The consumer reads it. This is one cache-line transfer per batch, not per item, so it's cheap. rb_count() becomes head_snapshot - consumer_pos.

B. Disable rb_count(). Return RB_ERR_NOT_SUPPORTED or a documented approximation. If your workload doesn't need the count, this is cleanest.

C. Sample via the last unconsumed slot. Walk forward from consumer_pos until you find a slot whose seq isn't pos+1. That count is exact but O(N) worst case. Use it only for diagnostics.

I'd do A — one atomic store per notify call, which you already do for the futex.
rb_wait() / futex word

In mode 0, the futex word is head — monotonic, always changing. In mode 1, producer_pos is private. So you need a separate monotonic word for notification only. That's notify_seq in the sketch above. The producer updates it once per publish (or once per batch — same optimization as head_snapshot). The consumer does rb_wait(rb, v, timeout) on that word.

Same pattern, different variable name.
Full/empty detection

Mode 0: head - tail >= limit means full, head == tail means empty.
Mode 1: slot's seq tells you directly. Full means the slot at producer_pos still has seq pos - 1 (consumer hasn't released it yet). Empty means the slot at consumer_pos still has seq pos - N (producer hasn't written yet).

No subtraction needed. Just one load and one compare.
When to use which
Workload	Mode
Same process, memcpy on critical path	0
Same process, DMA offload	1
Cross-NUMA (multi-socket)	1
Zynq / FPGA control plane	1
GPU with pinned memory	1
Need rb_count() frequently	0
Need zero complexity overhead	0
Small payloads, ring-bound	1
Large payloads, memcpy-bound	0 or 1 (marginal)
How to structure the code

A single compile-time switch in rb_config.h:
c

#ifndef RB_PER_SLOT_LAP
#define RB_PER_SLOT_LAP 0
#endif

Then in rb.c, every place that touches head/tail gets:
c

#if RB_PER_SLOT_LAP
    /* per-slot path */
#else
    /* shared-counter path */
#endif

There are roughly 8 such blocks. The rest — init, config, callbacks, stats, backpressure — is shared.

The port layer (rb_port.h) doesn't change. The API header (rb.h) doesn't change, except a note that rb_count() behaves differently.
The honest summary of what you gain

Mode 0, 2 KB payloads, single socket:

    Ring cycle: ~14 ns

    Total per item: ~99 ns

    Ring is 14% of the item cost

Mode 1, same workload:

    Ring cycle: ~8 ns (one less cache-line transfer)

    Total per item: ~93 ns

    6% end-to-end improvement

Mode 1, Zynq ARM-only path (no memcpy on critical path):

    Mode 0 ring cycle: ~14 ns of ARM work

    Mode 1 ring cycle: ~8 ns of ARM work

    43% improvement in the ARM's per-item cost

Mode 1, cross-NUMA at 300 cycles per transfer:

    Mode 0 ring cycle: ~350 ns

    Mode 1 ring cycle: ~180 ns

    49% improvement

Same code. Different workload. Different winner.
The implementation plan

    Add RB_PER_SLOT_LAP to rb_config.h, default 0.

    Add slot_header type and adjust RB_SLOT_HDR_SIZE based on the flag.

    Add producer_pos, consumer_pos, notify_seq to struct rb_s under the flag.

    Wrap the 8 hot-path blocks with #if RB_PER_SLOT_LAP.

    Adjust rb_init to initialize the seq values: slot[i].seq = i for i = 0..N-1.

    Make rb_count() and rb_wait() use notify_seq in mode 1.

    Add a test target test_rb_per_slot compiled with -DRB_PER_SLOT_LAP=1 that runs the same test suite.

    Document both modes in docs/TECHNICAL.md with the tradeoff table above.

The API is identical. Callers don't know which mode they're in. That's the point: the choice is a build-time optimization, not an API change.
