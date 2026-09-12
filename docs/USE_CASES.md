# Ringbuffer + Scratchpad — Use Cases

This document is example-oriented: short scenarios, code sketches, and the specific reason the ringbuffer + scratchpad design is useful.

The central pattern is:

```text
producer                         consumer
   │                                ▲
   │ write payload                  │ read payload
   ▼                                │
┌──────────────────────────────────────────────┐
│                 scratchpad                   │
│ [ slot 0 ][ slot 1 ][ slot 2 ][ ... ]       │
└──────────────────────────────────────────────┘
       │              ▲
       │ slot index   │ slot index
       ▼              │
   ┌───────────────────────┐
   │       SPSC ring       │
   │ [ 4 ][ 7 ][ 2 ][ ...] │
   └───────────────────────┘
```

The ring transports ownership/order information. The payload remains in its preallocated slot.

---

## 1. Network packet receive path

### Conventional copy path

```c
recv(fd, packet, packet_len, 0);
memcpy(ring_slot, packet, packet_len);
publish(ring_slot);
```

### Scratchpad path

```c
uint32_t slot;
void *buf;
uint32_t capacity;

if (rb_acquire(rb, expected_len, &slot, &buf, &capacity) == RB_OK) {
    ssize_t n = recv(fd, buf, capacity, 0);
    if (n > 0)
        rb_publish(rb, slot, (uint32_t)n);
    else
        rb_abort(rb, slot);
}
```

### Advantage

```text
socket/NIC → scratch slot → consumer
                  │
                  └── no intermediate payload copy
```

Useful for:

- Ethernet frames
- UDP/TCP message framing
- packet capture
- protocol parsers
- high-rate telemetry

For 1500-byte or larger messages, removing a payload `memcpy()` can matter more than optimizing the queue itself.

---

## 2. DMA / embedded I/O

The scratchpad can be used as the destination of a DMA transfer.

```text
ADC / UART / SPI / Ethernet DMA
              │
              ▼
        scratch slot N
              │
        DMA completion
              │
              ▼
        rb_publish(N)
              │
              ▼
             DSP
```

Sketch:

```c
rb_acquire(rb, DMA_BLOCK_SIZE, &slot, &buf, &capacity);

start_dma_rx(buf, DMA_BLOCK_SIZE, slot);

/* DMA completion callback/task */
rb_publish(rb, slot, received_bytes);
```

### Advantage

The usual path:

```text
DMA buffer → memcpy() → ring buffer → consumer
```

becomes:

```text
DMA → ring-owned scratch slot → consumer
```

No allocator is required, and the memory footprint is bounded at initialization.

Particularly useful for bare-metal and RTOS designs where deterministic allocation and bounded latency matter.

---

## 3. Audio blocks / DSP

A producer can generate an entire audio block directly in the acquired slot.

```c
uint32_t slot;
float *samples;
uint32_t capacity;

if (rb_acquire(rb, BLOCK_BYTES, &slot,
               (void **)&samples, &capacity) == RB_OK) {
    process_audio_block(samples, BLOCK_SAMPLES);
    rb_publish(rb, slot, BLOCK_BYTES);
}
```

```text
ADC / capture
     │
     ▼
 scratch[3]
     │
     ▼
    ring
     │
     ▼
 DSP / encoder / output
```

### Advantage

A 2048-sample block does not have to be copied into another queue-owned array.

The useful unit being passed is the **buffer ownership**, not the samples themselves.

Good candidates:

- audio capture
- DSP pipelines
- FFT blocks
- resampling
- codecs
- network audio

For small messages the advantage is smaller; it becomes increasingly relevant as block size grows.

---

## 4. Camera / video frames

Large payloads make the zero-copy property particularly visible.

```text
camera / DMA
     │
     ▼
┌──────────────┐
│ scratch slot │   e.g. one complete frame
└──────────────┘
     │
     ▼
   ring index
     │
     ▼
 color conversion / encoder
```

Instead of:

```c
capture(frame_a);
memcpy(ring_frame, frame_a, FRAME_SIZE);
consume(ring_frame);
```

the capture destination can be the slot itself.

```c
rb_acquire(rb, FRAME_SIZE, &slot, &frame, &capacity);
camera_capture_into(frame, FRAME_SIZE);
rb_publish(rb, slot, FRAME_SIZE);
```

### Advantage

For multi-megabyte frames, the queue metadata remains tiny while the payload stays stationary.

This pattern also scales naturally to pipelines:

```text
capture → ring → processing → ring → encoder
```

where each stage transfers buffer ownership instead of copying the frame.

---

## 5. Logging / telemetry

The acquired memory can be the final formatted record.

### Temporary-buffer approach

```c
char tmp[2048];
int n = snprintf(tmp, sizeof(tmp), "value=%d ...", value);
memcpy(ring_slot, tmp, n);
```

### Scratchpad approach

```c
uint32_t slot;
char *dst;
uint32_t capacity;

if (rb_acquire(rb, 2048, &slot, (void **)&dst, &capacity) == RB_OK) {
    int n = snprintf(dst, capacity, "value=%d ...", value);
    if (n >= 0 && (uint32_t)n < capacity)
        rb_publish(rb, slot, (uint32_t)n);
    else
        rb_abort(rb, slot);
}
```

### Advantage

The formatting operation writes directly into the storage consumed by the logger.

That can eliminate both:

1. a temporary buffer
2. the subsequent payload copy

Useful for high-rate diagnostics where formatting itself is already part of the producer cost.

---

## 6. Serialization / message construction

The producer does not necessarily know the final message size until it has constructed it.

```c
rb_acquire(rb, MAX_MESSAGE, &slot, &buf, &capacity);

uint32_t used = serialize_message(buf, capacity, &message);

if (used != 0)
    rb_publish(rb, slot, used);
else
    rb_abort(rb, slot);
```

The sequence is:

```text
reserve → construct in place → publish actual length
```

rather than:

```text
construct temporary → calculate size → copy → publish
```

Useful for:

- binary protocols
- JSON/text generation
- telemetry records
- IPC messages
- command packets
- compressed blocks

---

## 7. Packet-processing pipeline

A sequence of SPSC queues can form a lightweight processing pipeline.

```text
RX
 │
 ▼
[ring A]
 │
 ▼
parser
 │
 ▼
[ring B]
 │
 ▼
validator
 │
 ▼
[ring C]
 │
 ▼
application
```

If the payload storage is designed for ownership transfer, stages can operate on buffers without repeatedly copying the packet.

Conceptually:

```text
             payload stays here
                    │
                    ▼
        ┌─────────────────────┐
        │ fixed buffer pool   │
        └─────────────────────┘
          ▲       ▲       ▲
          │       │       │
        ring A  ring B  ring C
```

### Advantage

The queue becomes a very small synchronization/ordering mechanism while the expensive object stays in memory.

This is particularly attractive when each processing stage is CPU-bound rather than copy-bound.

---

## 8. Fixed-memory embedded message queues

A conventional message queue often combines queue metadata and message storage.

This design separates them:

```text
control / indices
      +
N fixed-size payload slots
```

Example configuration:

```c
rb_config_t cfg = {
    .capacity = 64,
    .slot_size = 2048,
    /* ... */
};
```

Conceptually:

```text
64 slots × 2048 bytes
= 128 KiB payload storage

plus small ring metadata
```

### Advantage

Memory consumption is known before runtime.

No per-message allocation means:

- no heap fragmentation
- no allocator lock/contention
- deterministic lifetime
- predictable maximum queue depth
- straightforward static/preallocated deployments

---

## 9. High-throughput producer / consumer

For a large payload, compare the amount of data moved by the two designs.

### Conventional payload ring

```text
producer
   │
   │ memcpy(payload)
   ▼
ring storage
   │
   ▼
consumer
```

### Scratchpad ring

```text
producer
   │
   │ write once
   ▼
scratch slot
   │
   │ publish index
   ▼
consumer
```

The ring operation itself is independent of payload size.

The payload still has to be written once, but it does not have to be **moved again merely to enqueue it**.

This is most valuable when:

```text
payload size >> queue metadata size
```

For example:

```text
16-byte descriptor
        vs.
2048-byte payload
```

The queue moves the former; the latter remains in place.

---

## 10. Ownership-transfer API

The design is useful when a buffer is better thought of as an object whose ownership changes between threads.

```text
producer owns slot
       │
       │ rb_publish()
       ▼
consumer owns slot
       │
       │ rb_release()
       ▼
slot becomes reusable
```

A conventional queue often models the operation as:

```text
copy object → queue
copy object ← queue
```

This design models it as:

```text
transfer ownership → transfer ownership
```

That distinction is useful for large or externally-produced objects.

---

## When a normal ringbuffer is preferable

The scratchpad design is not universally better.

A conventional byte ring can be preferable when:

- messages are very small
- payloads are naturally variable-sized
- storage utilization matters more than fixed-slot simplicity
- the producer already owns a temporary buffer and a copy is unavoidable
- random/out-of-order release is required
- the workload is not SPSC

For example, for a stream of 8-byte values:

```text
memcpy(8 bytes)
```

may be cheaper and simpler than maintaining a 2048-byte slot per logical message.

The scratchpad approach becomes increasingly attractive as the payload becomes larger, more expensive to construct, or naturally produced into a writable buffer.

---

## Design summary

The useful mental model is:

```text
                SPSC ring
          ┌──────────────────┐
producer ─► slot ownership   ├─► consumer
          └──────────────────┘
                    │
                    │ index
                    ▼
          ┌──────────────────┐
          │    scratchpad    │
          │                  │
          │ payload payload  │
          │ payload payload  │
          └──────────────────┘
```

The ring transports **ordering and ownership**.

The scratchpad contains **the actual data**.

That separation is the main reason the design can outperform a payload-copying ring for large messages and can provide a more deterministic memory model in embedded and high-throughput systems.
