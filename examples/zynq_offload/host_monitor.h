#ifndef HOST_MONITOR_H
#define HOST_MONITOR_H

#include <stdint.h>

struct host_snapshot {
    uint64_t timestamp_ns;
    uint64_t cpu_user;
    uint64_t cpu_nice;
    uint64_t cpu_system;
    uint64_t cpu_idle;
    uint64_t cpu_iowait;
    uint64_t cpu_irq;
    uint64_t cpu_softirq;
    uint64_t cpu_steal;
    uint64_t mem_total_kb;
    uint64_t mem_available_kb;
    double load1;
    double load5;
    double load15;
};

void host_print_identity(void);
int host_snapshot_take(struct host_snapshot *snapshot);
void host_print_delta(const struct host_snapshot *start,
                      const struct host_snapshot *end,
                      double elapsed_sec);

#endif
