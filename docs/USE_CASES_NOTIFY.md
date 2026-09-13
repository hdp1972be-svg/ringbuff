# Notification Use Cases — Ringbuffer + Scratchpad

`RB_ENABLE_NOTIFY=1` adds an optional Linux notification path without changing the normal build. The producer still publishes through the same SPSC ring; notification is only a wake-up hint.

## Sleeping consumer

A consumer that would otherwise continuously poll can sleep while the ring is empty and be woken when a producer publishes. The consumer should always recheck the ring after waking. The futex uses the ring head sequence as its expected-value word, so a publication racing with entry into the wait cannot leave the consumer asleep on an obsolete sequence.

This is useful for bursty telemetry, logging, IPC, DMA completion, camera capture, NIC receive paths and FPGA pipelines where an idle consumer should not consume a CPU core.

## libuv / libev / epoll

A futex is not a file descriptor. Event-loop based consumers should use the optional eventfd returned by `rb_notify_fd()`. Register that fd with the event loop, drain it when readable with `rb_notify_drain_fd()`, then drain the ring.

The eventfd is nonblocking and suitable for poll/epoll-style fd monitoring. Publication signals when the queue changes from empty to non-empty, so a burst of messages can be handled by one event-loop callback instead of one callback per message.

## Hybrid spin-then-sleep

Latency-sensitive consumers can spin for a short bounded period and then enter `rb_wait()`. This provides a useful middle ground between permanent busy polling and sleeping immediately on every idle interval.

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

## Semantics

Notification does not transfer ownership and does not replace the ring memory-ordering protocol. The producer still writes the payload, publishes the descriptor with the existing release-store, and then signals. The consumer still observes the published head, consumes the payload, and releases the slot.

A notification is therefore only a wake-up hint. Correctness must never depend on receiving exactly one notification per message.

## Build

Enable the feature with CMake using `-DRB_ENABLE_NOTIFY=ON`, or compile with `-DRB_ENABLE_NOTIFY=1` on Linux. The default remains disabled, so the normal build keeps the original portable fast path without notification state or system calls.
