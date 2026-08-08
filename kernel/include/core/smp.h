#ifndef _CORE_SMP_H
#define _CORE_SMP_H

#include "core/cpu.h"

/*
 * Architecture-independent SMP topology and startup interface.  Logical CPU
 * IDs are dense, start at zero, and are limited to the 32-bit scheduler mask.
 * Hardware IDs and platform cookies are opaque to common code.
 */

#if CONFIG_NR_CPUS > 32
#error "The shared SMP CPU mask supports at most 32 CPUs"
#endif

typedef struct smp_cpu_desc {
    unsigned logical_id;
    uint64_t hw_id;
    uintptr_t platform_cookie;
} smp_cpu_desc_t;

typedef enum smp_ipi_reason {
    SMP_IPI_RESCHEDULE = 0,
    SMP_IPI_TLB_FLUSH = 1,
} smp_ipi_reason_t;

typedef struct smp_platform_ops {
    /* Fill and return the CPUs this kernel can manage, up to capacity. */
    unsigned (*discover)(smp_cpu_desc_t *cpus, unsigned capacity,
                         uint64_t boot_hw_id);
    int (*start)(const smp_cpu_desc_t *cpu, uintptr_t entry_pa,
                 uintptr_t logical_context);
    void (*send_ipi)(const smp_cpu_desc_t *cpu, smp_ipi_reason_t reason);
    int (*remote_tlb_flush)(uint32_t logical_mask, uint64_t addr,
                            uint64_t size);
    /* Per-CPU controller or firmware setup, run on the secondary CPU. */
    void (*secondary_init)(const smp_cpu_desc_t *cpu);
} smp_platform_ops_t;

/* Send a reschedule IPI to an online logical CPU. */
void smp_send_reschedule(unsigned cpu);
int smp_remote_tlb_flush(uint32_t logical_mask, uint64_t addr, uint64_t size);
int smp_remote_tlb_flush_supported(void);

/* Discover topology after proc_init(). */
void smp_init(void);

/* Start configured secondary CPUs; failures leave those CPUs offline. */
void smp_boot_secondaries(void);

/* Secondary CPU initialization entry, called by architecture entry code. */
void smp_secondary_init(unsigned cpu_id);

/* Online-state helpers used by the shared lifecycle and status APIs. */
void smp_cpu_mark_online(unsigned cpu);
/* CPUs discovered and described by the selected platform, capped by the
 * build-time capacity. This is not a count of ignored physical CPUs. */
unsigned smp_present_cpu_count(void);
unsigned smp_configured_cpu_count(void);
unsigned smp_online_cpu_count(void);
uint32_t smp_online_cpu_mask(void);
int smp_cpu_is_online(unsigned cpu);
const smp_cpu_desc_t *smp_cpu_desc(unsigned logical_id);
int smp_hw_to_logical(uint64_t hw_id, unsigned *logical_id);
int smp_logical_to_hw(unsigned logical_id, uint64_t *hw_id);

/* Minimal architecture hooks used by the shared startup path. */
uint64_t arch_smp_boot_hw_id(void);
uintptr_t arch_smp_secondary_entry_pa(void);
int arch_smp_secondary_prepare(const smp_cpu_desc_t *cpu);

#endif /* _CORE_SMP_H */
