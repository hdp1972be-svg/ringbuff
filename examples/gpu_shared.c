#include "rb.h"

#include <cuda_runtime.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Scratchpad allocated with cudaHostAlloc(cudaHostAllocMapped).
 *
 * The same physical pages are addressable from the CPU (host pointer)
 * and from the GPU (device pointer returned by cudaHostGetDevicePointer).
 * Both sides see the same bytes at the same physical addresses. No
 * copy, no staging.
 *
 * The ring control block stays in regular heap memory. It only holds
 * counters that the CPU reads and writes, and the GPU never needs to
 * see it.
 *
 * On systems without a CUDA device this example cannot run. CMake
 * detects CUDAToolkit and skips the target if it is not found.
 */

#define CAPACITY    32u
#define SLOTS       32u
#define SLOT_SIZE   4096u

static void *xmalloc(size_t sz) {
    void *p = malloc(sz);
    if (!p) { fprintf(stderr, "oom\n"); exit(2); }
    return p;
}

int main(void) {
    int devcount = 0;
    if (cudaGetDeviceCount(&devcount) != cudaSuccess || devcount == 0) {
        fprintf(stderr, "no CUDA device found\n");
        return 1;
    }

    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, 0);
    printf("device 0: %s (compute %d.%d, %zu MB)\n",
           prop.name, prop.major, prop.minor,
           (size_t)(prop.totalGlobalMem / (1024 * 1024)));
    printf("canMapHostMemory: %s\n",
           prop.canMapHostMemory ? "yes" : "no");

    if (!prop.canMapHostMemory) {
        fprintf(stderr, "device does not support mapped host memory\n");
        return 1;
    }

    cudaSetDeviceFlags(cudaDeviceMapHost);

    /* ---- scratchpad: pinned, mappable host memory ---- */
    size_t scratch_size = (size_t)SLOTS * SLOT_SIZE;
    void *scratch_host = NULL;
    cudaError_t rc = cudaHostAlloc(&scratch_host, scratch_size,
                                   cudaHostAllocMapped);
    if (rc != cudaSuccess) {
        fprintf(stderr, "cudaHostAlloc: %s\n", cudaGetErrorString(rc));
        return 1;
    }

    void *scratch_dev = NULL;
    rc = cudaHostGetDevicePointer(&scratch_dev, scratch_host, 0);
    if (rc != cudaSuccess) {
        fprintf(stderr, "cudaHostGetDevicePointer: %s\n",
               cudaGetErrorString(rc));
        cudaFreeHost(scratch_host);
        return 1;
    }

    printf("scratchpad: host=%p  device=%p  size=%zu B\n",
           scratch_host, scratch_dev, scratch_size);

    /* ---- ring control block: regular heap ---- */
    rb_t *rb = (rb_t *)xmalloc(rb_size(CAPACITY));

    rb_config_t cfg;
    rb_config_init(&cfg);
    cfg.capacity  = CAPACITY;
    cfg.slots     = SLOTS;
    cfg.slot_size = SLOT_SIZE;

    if (rb_init(rb, &cfg, scratch_host, scratch_size) != RB_OK) {
        fprintf(stderr, "rb_init failed\n");
        return 1;
    }

    /* ---- CPU producer writes 3 messages into slots ---- */
    for (int i = 0; i < 3; ++i) {
        uint32_t idx, cap;
        void *w;
        if (rb_acquire(rb, 64, &idx, &w, &cap) != RB_OK) break;
        int len = snprintf(w, 64, "hello from CPU, item %d", i);
        rb_publish(rb, idx, (uint32_t)len);
    }

    printf("producer: 3 messages published into slots 0..2\n");

    /* ---- GPU-side view: allocate a device buffer, cudaMemcpy the
       scratchpad through it, back to host. This is a stand-in for
       "a kernel processed the payloads." In real use you would launch
       a kernel that reads from scratch_dev directly. ---- */
    void *staging = NULL;
    rc = cudaMalloc(&staging, scratch_size);
    if (rc == cudaSuccess) {
        cudaMemcpy(staging, scratch_dev, scratch_size,
                   cudaMemcpyDeviceToDevice);
        cudaMemcpy(scratch_host, staging, scratch_size,
                   cudaMemcpyDeviceToHost);
        cudaFree(staging);
        printf("gpu: round-tripped %zu B through device memory\n",
               scratch_size);
    } else {
        fprintf(stderr, "cudaMalloc: %s\n", cudaGetErrorString(rc));
    }

    /* ---- CPU consumer reads the messages back ---- */
    uint32_t idx, len;
    const void *obj;
    bool trunc;
    int seen = 0;
    while (rb_consume(rb, &idx, &obj, &len, &trunc) == RB_OK) {
        printf("consumer: slot %u len %u: %.*s\n",
               idx, len, (int)(len < 40 ? len : 40), (const char *)obj);
        rb_release(rb, idx);
        seen++;
    }
    printf("consumer: %d messages, data intact after GPU round trip\n", seen);

    rb_deinit(rb);
    free(rb);
    cudaFreeHost(scratch_host);
    return 0;
}
