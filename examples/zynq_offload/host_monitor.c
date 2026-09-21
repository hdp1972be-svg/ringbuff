#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "host_monitor.h"

#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns_local(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int read_cpu_stat(struct host_snapshot *s)
{
    FILE *f = fopen("/proc/stat", "r");
    char line[512];
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;

    if (!f)
        return -1;

    if (!fgets(line, sizeof(line), f) ||
        sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
               &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal) != 8) {
        fclose(f);
        return -1;
    }
    fclose(f);

    s->cpu_user = user;
    s->cpu_nice = nice;
    s->cpu_system = system;
    s->cpu_idle = idle;
    s->cpu_iowait = iowait;
    s->cpu_irq = irq;
    s->cpu_softirq = softirq;
    s->cpu_steal = steal;
    return 0;
}

static void read_meminfo(struct host_snapshot *s)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char key[64];
    unsigned long long value;
    char unit[16];

    s->mem_total_kb = 0;
    s->mem_available_kb = 0;

    if (!f)
        return;

    while (fscanf(f, "%63[^:]: %llu %15s\n", key, &value, unit) == 3) {
        if (!strcmp(key, "MemTotal"))
            s->mem_total_kb = value;
        else if (!strcmp(key, "MemAvailable"))
            s->mem_available_kb = value;
    }
    fclose(f);
}

static void read_loadavg(struct host_snapshot *s)
{
    FILE *f = fopen("/proc/loadavg", "r");
    s->load1 = s->load5 = s->load15 = 0.0;
    if (f) {
        (void)fscanf(f, "%lf %lf %lf", &s->load1, &s->load5, &s->load15);
        fclose(f);
    }
}

static unsigned count_cpuinfo(const char *field)
{
    FILE *f = fopen("/proc/cpuinfo", "r");
    char line[512];
    unsigned count = 0;

    if (!f)
        return 0;

    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, field, strlen(field)) && strchr(line, ':'))
            count++;
    }
    fclose(f);
    return count;
}

static void read_cpu_model(char *buf, size_t size)
{
    FILE *f = fopen("/proc/cpuinfo", "r");
    char line[512];

    if (!f || size == 0) {
        if (size)
            snprintf(buf, size, "unknown");
        if (f)
            fclose(f);
        return;
    }

    buf[0] = '\0';
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "model name", 10) ||
            !strncmp(line, "Hardware", 8) ||
            !strncmp(line, "Processor", 9)) {
            char *p = strchr(line, ':');
            if (p) {
                ++p;
                while (*p == ' ' || *p == '\t')
                    ++p;
                p[strcspn(p, "\r\n")] = '\0';
                snprintf(buf, size, "%s", p);
                break;
            }
        }
    }
    fclose(f);

    if (!buf[0])
        snprintf(buf, size, "unknown");
}

static void print_affinity(void)
{
    cpu_set_t set;
    int n = 0;
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
            if (CPU_ISSET(cpu, &set))
                ++n;
        printf("  affinity: %d logical CPU(s)", n);
        if (n > 0) {
            printf(" [");
            int first = 1;
            for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
                if (CPU_ISSET(cpu, &set)) {
                    if (!first)
                        printf(",");
                    printf("%d", cpu);
                    first = 0;
                }
            }
            printf("]");
        }
        printf("\n");
    } else {
        printf("  affinity: unavailable (%s)\n", strerror(errno));
    }
}

static const char *policy_name(int policy)
{
    switch (policy) {
    case SCHED_OTHER: return "SCHED_OTHER";
    case SCHED_FIFO: return "SCHED_FIFO";
    case SCHED_RR: return "SCHED_RR";
#ifdef SCHED_BATCH
    case SCHED_BATCH: return "SCHED_BATCH";
#endif
#ifdef SCHED_IDLE
    case SCHED_IDLE: return "SCHED_IDLE";
#endif
#ifdef SCHED_DEADLINE
    case SCHED_DEADLINE: return "SCHED_DEADLINE";
#endif
    default: return "unknown";
    }
}

void host_print_identity(void)
{
    struct utsname u;
    char model[256];
    long nconf = sysconf(_SC_NPROCESSORS_CONF);
    long nonline = sysconf(_SC_NPROCESSORS_ONLN);
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGESIZE);
    int policy = sched_getscheduler(0);
    int nice = getpriority(PRIO_PROCESS, 0);
    unsigned cpuinfo_processors = count_cpuinfo("processor");

    if (uname(&u) != 0)
        memset(&u, 0, sizeof(u));
    read_cpu_model(model, sizeof(model));

    printf("\n=== Host identity ===\n");
    printf("  CPU: %s\n", model);
    printf("  logical CPUs: configured=%ld online=%ld /proc/cpuinfo=%u\n",
           nconf, nonline, cpuinfo_processors);
    if (pages > 0 && page_size > 0)
        printf("  physical RAM: %.1f MiB\n",
               (double)pages * (double)page_size / (1024.0 * 1024.0));
    printf("  kernel: %s %s %s\n", u.sysname, u.release, u.machine);
    printf("  scheduler: %s (%d), nice=%d\n",
           policy >= 0 ? policy_name(policy) : "unknown", policy, nice);
    print_affinity();
    printf("  PID: %ld\n", (long)getpid());
    printf("=====================\n\n");
    fflush(stdout);
}

int host_snapshot_take(struct host_snapshot *s)
{
    if (!s)
        return -1;

    memset(s, 0, sizeof(*s));
    s->timestamp_ns = now_ns_local();
    (void)read_cpu_stat(s);
    read_meminfo(s);
    read_loadavg(s);
    return 0;
}

void host_print_delta(const struct host_snapshot *a,
                      const struct host_snapshot *b,
                      double elapsed_sec)
{
    uint64_t busy_a, busy_b, total_a, total_b, d_busy, d_total;
    double cpu_pct = 0.0;
    double mem_used_pct = 0.0;
    double avg_load = 0.0;

    if (!a || !b)
        return;

    busy_a = a->cpu_user + a->cpu_nice + a->cpu_system +
             a->cpu_irq + a->cpu_softirq + a->cpu_steal;
    busy_b = b->cpu_user + b->cpu_nice + b->cpu_system +
             b->cpu_irq + b->cpu_softirq + b->cpu_steal;
    total_a = busy_a + a->cpu_idle + a->cpu_iowait;
    total_b = busy_b + b->cpu_idle + b->cpu_iowait;

    d_busy = busy_b >= busy_a ? busy_b - busy_a : 0;
    d_total = total_b >= total_a ? total_b - total_a : 0;
    if (d_total)
        cpu_pct = 100.0 * (double)d_busy / (double)d_total;

    if (b->mem_total_kb)
        mem_used_pct = 100.0 *
            (double)(b->mem_total_kb - b->mem_available_kb) /
            (double)b->mem_total_kb;

    if (elapsed_sec > 0.0)
        avg_load = (a->load1 + b->load1) * 0.5;

    printf("\n=== Host load during benchmark ===\n");
    printf("  elapsed: %.3f s\n", elapsed_sec);
    printf("  aggregate CPU busy: %.2f%%\n", cpu_pct);
    printf("  load average: start %.2f  end %.2f  (5m end %.2f, 15m end %.2f)\n",
           a->load1, b->load1, b->load5, b->load15);
    printf("  avg sampled 1m load: %.2f\n", avg_load);
    printf("  memory used: %.1f%%  available: %.1f MiB / %.1f MiB\n",
           mem_used_pct,
           (double)b->mem_available_kb / 1024.0,
           (double)b->mem_total_kb / 1024.0);
    printf("===================================\n");
}
