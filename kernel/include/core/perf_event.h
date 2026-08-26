#ifndef _CORE_PERF_EVENT_H
#define _CORE_PERF_EVENT_H

#include <stdint.h>

/*
 * Kernel-internal software perf-event objects (kernel/core/perf_event.c).
 *
 * Counter objects, their vfile ops and enable/disable accounting are core
 * infrastructure sourced from per-task counters kept by the fault handler
 * and scheduler; the Linux ABI layer only decodes the perf_event_attr wire
 * struct onto these calls.  Config/format/ioctl values match the Linux
 * wire because they are stored and reported verbatim.
 */

#define PERF_COUNT_SW_CPU_CLOCK       0
#define PERF_COUNT_SW_TASK_CLOCK      1
#define PERF_COUNT_SW_PAGE_FAULTS     2
#define PERF_COUNT_SW_CONTEXT_SWITCHES 3
#define PERF_COUNT_SW_CPU_MIGRATIONS  4
#define PERF_COUNT_SW_PAGE_FAULTS_MIN 5
#define PERF_COUNT_SW_PAGE_FAULTS_MAJ 6
#define PERF_COUNT_SW_ALIGNMENT_FAULTS 7
#define PERF_COUNT_SW_EMULATION_FAULTS 8
#define PERF_COUNT_SW_DUMMY           9
#define PERF_COUNT_SW_BPF_OUTPUT      10
#define PERF_COUNT_SW_CGROUP_SWITCHES 11

#define PERF_FORMAT_TOTAL_TIME_ENABLED (1ULL << 0)
#define PERF_FORMAT_TOTAL_TIME_RUNNING (1ULL << 1)
#define PERF_FORMAT_ID                 (1ULL << 2)
#define PERF_FORMAT_GROUP              (1ULL << 3)
#define PERF_FORMAT_LOST               (1ULL << 4)

#define PERF_EVENT_IOC_ENABLE    0x2400
#define PERF_EVENT_IOC_DISABLE   0x2401
#define PERF_EVENT_IOC_REFRESH   0x2402
#define PERF_EVENT_IOC_RESET     0x2403
#define PERF_EVENT_IOC_PERIOD    0x80072404UL
#define PERF_EVENT_IOC_SET_OUTPUT 0x2405
#define PERF_EVENT_IOC_ID        0x40072407UL

/* Returns nonzero when a software counter source exists for config. */
int perf_event_config_supported(uint64_t config);

/* Create an event fd for a resolved target pid (-1 = system-wide).
 * Returns the installed anonymous fd or a negative errno. */
int perf_event_create_fd(uint64_t config, uint64_t read_format, int pid,
                         int cpu, uint64_t sample_period, int disabled,
                         int cloexec);

#endif /* _CORE_PERF_EVENT_H */
