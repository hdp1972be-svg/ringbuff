# Notification Use Cases — Ringbuffer + Scratchpad

`RB_ENABLE_NOTIFY=1` adds an optional Linux notification path without changing the normal build. The producer still publishes through the same SPSC ring; notification is only a wake-up hint.

## Sleeping consumer — futex

A consumer that would otherwise continuously poll can sleep while the ring is empty and be woken when a producer publishes. The futex uses the ring's `head` sequence as its expected-value word, so a publication racing with entry into the wait cannot leave the consumer asleep on an obsolete sequence.

The basic pattern is:

```c
for (;;) {
    if (rb_drain(rb, consume_one, ctx) > 0)
        continue;

    /* Snapshot the sequence before sleeping. */
    uint32_t expected = rb_notify_value(rb);

    /* Recheck before sleeping: the producer may have raced us. */
    if (rb_count(rb) != 0)
        continue;

    int rc = rb_wait(rb, expected, -1); /* -1 = wait indefinitely */
    if (rc < 0 && rc != -EINTR)
        break;
}
```

`rb_wait()` returns when the sequence changes, when the wait is interrupted, or when the optional timeout expires. The consumer should always recheck the ring after returning; notification does not carry message ownership or payload data.

This is useful for bursty telemetry, logging, IPC, DMA completion, camera capture, NIC receive paths and FPGA pipelines where an idle consumer should not consume a CPU core.

## libuv / libev / epoll — eventfd

A futex is not a file descriptor. Event-loop based consumers should use the optional eventfd returned by `rb_notify_fd()`. Register that fd with the event loop, drain it when readable with `rb_notify_drain_fd()`, then drain the ring.

Example with a generic `poll()` loop:

```c
int fd = rb_notify_fd(rb);

struct pollfd pfd = {
    .fd = fd,
    .events = POLLIN
};

for (;;) {
    if (poll(&pfd, 1, -1) < 0)
        break;

    if (pfd.revents & POLLIN) {
        rb_notify_drain_fd(rb);
        rb_drain(rb, consume_one, ctx);
    }
}
```

The same fd can be registered with `epoll`, `uv_poll_t`, or a similar fd-based event loop. The eventfd is nonblocking and notification is edge-like: a publication signals the transition from empty to non-empty. A burst of messages can therefore be handled by one event-loop callback instead of one callback per message.

After the callback drains the queue, the next empty-to-non-empty transition arms another notification.

## Hybrid spin-then-sleep

Latency-sensitive consumers can spin for a short bounded period and then enter `rb_wait()`. This provides a useful middle ground between permanent busy polling and sleeping immediately on every idle interval.

For example:

```c
for (unsigned spin = 0; spin != 100; ++spin) {
    if (rb_count(rb) != 0)
        goto drain;
    cpu_relax();
}

uint32_t expected = rb_notify_value(rb);
if (rb_count(rb) == 0)
    rb_wait(rb, expected, 1000); /* bounded sleep */

drain:
rb_drain(rb, consume_one, ctx);
```

## Device-facing producers

DMA, FPGA, NIC and camera producers can publish ownership of a preallocated scratch slot and wake a sleeping CPU consumer. The payload remains in place; the notification only tells the consumer that it is worth checking the ring.

```text
DMA / FPGA / NIC / camera
          |
          v
   shared scratch slot
          |
       publish
          |
          v
      SPSC ring -----> futex / eventfd wake
          |
          v
      CPU consumer
```

For a hardware producer, the device completion/fence and any required cache maintenance must happen before the ring publication becomes visible to the consumer. `RB_ENABLE_NOTIFY` does not replace those device-specific ordering requirements.

## API summary

When `RB_ENABLE_NOTIFY=1` on Linux:

| API | Purpose |
|---|---|
| `rb_notify_value(rb)` | Read the current notification/head sequence for use with `rb_wait()` |
| `rb_wait(rb, expected, timeout_ms)` | Sleep on the sequence using a Linux futex |
| `rb_notify_fd(rb)` | Obtain the nonblocking eventfd for poll/epoll/libuv/libev integration |
| `rb_notify_drain_fd(rb)` | Drain the eventfd after it becomes readable |

The eventfd should be obtained during setup, before the producer is started. The returned descriptor belongs to the ring and is closed during ring teardown.

## Semantics

Notification does not transfer ownership and does not replace the ring memory-ordering protocol. The producer still writes the payload, publishes the descriptor with the existing release-store, and then signals. The consumer still observes the published head, consumes the payload, and releases the slot.

A notification is therefore only a wake-up hint. Correctness must never depend on receiving exactly one notification per message. Consumers should always drain/recheck the ring after a wake-up because multiple publications may have been coalesced.

## Build

Enable the feature with CMake using `-DRB_ENABLE_NOTIFY=ON`, or compile with `-DRB_ENABLE_NOTIFY=1` on Linux. The default remains disabled, so the normal build keeps the original portable fast path without notification state or system calls.

The feature is intentionally Linux-specific because it uses Linux futexes and `eventfd`. Other platforms can retain the normal ring implementation without enabling the notification option.
