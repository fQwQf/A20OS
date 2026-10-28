/* SPDX-License-Identifier: Apache-2.0
 * C906 cache instruction encodings: T-Head XTheadCmo specification and
 * U-Boot v2025.10 arch/riscv/lib/thead_cmo.c; SDK Linux cacheflush.c.
 * Enabled only after SG2002/C906/SBI identity and a recoverable S-mode probe.
 */
#include "cpu.h"
#include "dma_cache.h"
#include "trap_frame.h"
#include "boot/context.h"
#include "firmware/node.h"
#include "core/errno.h"
#include "core/panic.h"

static volatile int probe_active, probe_failed;
static int cache_ready;
static uint8_t probe_line[64] __attribute__((aligned(64)));
extern char riscv_cmo_probe_clean[], riscv_cmo_probe_invalidate[];
extern char riscv_cmo_probe_sync[], riscv_cmo_probe_resume[];

/* Called only by the architecture's kernel trap entry before generic dispatch.
 * All other illegal instructions, user traps and arbitrary PCs remain fatal. */
int riscv_c906_cache_probe_fixup(trap_context_t *ctx) {
#if defined(CONFIG_NR_CPUS) && CONFIG_NR_CPUS != 1
    /* prepare() disables this single-hart probe in SMP builds, so its inline
     * assembly labels are not emitted. Do not reference them from the fixup. */
    (void)ctx;
    return 0;
#else
    if (!probe_active || !(ctx->sstatus & (1UL << 8)) || arch_read_cause() != 2)
        return 0;
    uint64_t expected;
    if (ctx->sepc == (uintptr_t)riscv_cmo_probe_clean)
        expected = 0x0295000b;
    else if (ctx->sepc == (uintptr_t)riscv_cmo_probe_invalidate)
        expected = 0x02b5000b;
    else if (ctx->sepc == (uintptr_t)riscv_cmo_probe_sync)
        expected = 0x01b0000b;
    else
        return 0;
    uint64_t value = arch_read_tval();
    if (value && value != expected)
        return 0;
    probe_failed = 1;
    ctx->sepc = (uintptr_t)riscv_cmo_probe_resume;
    return 1;
#endif
}

static int current_cpu_is_c906(const fw_node_t *root) {
    fw_node_t cpus, child;
    int found = 0;
    for (int rc = fw_node_next_child(root, NULL, &cpus); !rc;
         rc = fw_node_next_child(root, &cpus, &cpus)) {
        const char *name = fw_node_name(&cpus);
        if (!name || name[0] != 'c' || name[1] != 'p' || name[2] != 'u' ||
            name[3] != 's' || name[4])
            continue;
        extern uint64_t __boot_hart_id;
        for (int cr = fw_node_next_child(&cpus, NULL, &child); !cr;
             cr = fw_node_next_child(&cpus, &child, &child)) {
            uint32_t id;
            if (!fw_node_read_u32(&child, "reg", &id) && id == __boot_hart_id &&
                fw_node_match_id(&child, "thead,c906") == 1)
                found = 1;
        }
    }
    return found;
}
static int platform_is_sg2002_c906(const fw_node_t *root) {
    return fw_node_match_id(root, "sophgo,sg2002") == 1 &&
           current_cpu_is_c906(root);
}
int riscv_sg2002_dma_cache_prepare(void) {
#if defined(CONFIG_NR_CPUS) && CONFIG_NR_CPUS != 1
    return -EOPNOTSUPP; /* This local cache contract has no remote-hart CMO. */
#endif
    if (cache_ready)
        return 0;
    const fw_node_t *root = &boot_context_get()->hardware_root;
    if (!platform_is_sg2002_c906(root))
        return -ENODEV;
    register uint64_t a0 __asm__("a0") = 0;
    register uint64_t a1 __asm__("a1") = 0;
    register uint64_t a6 __asm__("a6") = 4; /* SBI BASE get_mvendorid */
    register uint64_t a7 __asm__("a7") = 0x10;
    __asm__ __volatile__("ecall"
                         : "+r"(a0), "+r"(a1)
                         : "r"(a6), "r"(a7)
                         : "memory");
    if (a0 || a1 != 0x5b7)
        return -ENODEV;
    int enabled = arch_irqs_enabled();
    arch_local_irq_disable();
    probe_failed = 0;
    probe_active = 1;
    probe_line[0] = 1;
    a0 = (uintptr_t)probe_line - PAGE_OFFSET;
    __asm__ __volatile__(
        ".balign 4\n.globl "
        "riscv_cmo_probe_clean\nriscv_cmo_probe_clean:\n.long 0x0295000b\n"
        ".globl riscv_cmo_probe_invalidate\nriscv_cmo_probe_invalidate:\n.long "
        "0x02b5000b\n"
        ".globl riscv_cmo_probe_sync\nriscv_cmo_probe_sync:\n.long 0x01b0000b\n"
        ".globl riscv_cmo_probe_resume\nriscv_cmo_probe_resume:\n"
        : "+r"(a0)::"memory");
    probe_active = 0;
    if (!probe_failed)
        cache_ready = 1;
    if (enabled)
        arch_local_irq_enable();
    return cache_ready ? 0 : -EOPNOTSUPP;
}
static void cache_range(const void *address, size_t length, int cpu) {
    if (!cache_ready) {
        arch_mb();
        return;
    } /* coherent platform default */
    uint64_t first, last;
    if (!riscv_dma_physical_span((uintptr_t)address, length, PAGE_OFFSET,
                                 &first, &last))
        panic("RISC-V DMA: invalid direct-map cache range");
    register uint64_t at __asm__("a0") = first;
    for (;;) {
        if (cpu)
            __asm__ __volatile__(".long 0x02b5000b" : "+r"(at)::"memory");
        else
            __asm__ __volatile__(".long 0x0295000b" : "+r"(at)::"memory");
        if (at == last)
            break;
        at += 64;
    }
    __asm__ __volatile__(".long 0x01b0000b" ::: "memory");
}
void riscv_dma_sync_device(const void *address, size_t length) {
    cache_range(address, length, 0);
}
void riscv_dma_sync_cpu(const void *address, size_t length) {
    cache_range(address, length, 1);
}

/* DMA consumers on non-coherent Nano must not silently fall back to the
 * coherent-platform barrier path when firmware denies cache maintenance. */
int arch_device_dma_ready(void) {
#ifdef CONFIG_BOARD_LICHEERV_NANO
    return cache_ready;
#else
    return 1;
#endif
}
