#include "core/arch.h"
#include "core/klog.h"
#include "core/panic.h"
#include "core/sync.h"

#define RISCV64_ASID_BITS 16U
#define RISCV64_ASID_COUNT (1U << RISCV64_ASID_BITS)
#define RISCV64_ASID_WORD_BITS 64U
#define RISCV64_ASID_WORDS (RISCV64_ASID_COUNT / RISCV64_ASID_WORD_BITS)

uint64_t riscv64_asid_mask;
static mutex_t g_riscv64_asid_lock = MUTEX_INIT;
static uint64_t g_riscv64_asid_live[RISCV64_ASID_WORDS];
static uint64_t g_riscv64_asid_eligible[RISCV64_ASID_WORDS];
static uint32_t g_riscv64_asid_cursor = 1;
static uint8_t g_riscv64_asid_initialized;

static void riscv64_asid_allocator_init_locked(void)
{
    if (g_riscv64_asid_initialized)
        return;

    uint64_t old_satp = arch_read_satp();
    arch_write_satp(old_satp | (0xffffUL << 44));
    uint64_t implemented = (arch_read_satp() >> 44) & 0xffffUL;
    arch_write_satp(old_satp);
    arch_tlb_flush_local();

    riscv64_asid_mask = implemented;
    if (implemented) {
        for (uint32_t asid = 1; asid <= implemented; asid++)
            g_riscv64_asid_eligible[asid / RISCV64_ASID_WORD_BITS] |=
                1UL << (asid % RISCV64_ASID_WORD_BITS);
    }
    __atomic_store_n(&g_riscv64_asid_initialized, 1, __ATOMIC_RELEASE);
    kinfo("[MM] RISC-V ASID mask=0x%lx bits=%u\n",
          (unsigned long)implemented,
          implemented ? (unsigned)__builtin_popcountll(implemented) : 0);
}

uint32_t riscv64_asid_alloc(void)
{
    mutex_lock(&g_riscv64_asid_lock);
    riscv64_asid_allocator_init_locked();
    uint32_t max_asid = (uint32_t)riscv64_asid_mask;
    if (!max_asid) {
        mutex_unlock(&g_riscv64_asid_lock);
        return 0;
    }

    for (unsigned pass = 0; pass < 2; pass++) {
        for (uint32_t scanned = 0; scanned < max_asid; scanned++) {
            uint32_t asid = g_riscv64_asid_cursor++;
            if (g_riscv64_asid_cursor > max_asid)
                g_riscv64_asid_cursor = 1;
            uint64_t bit = 1UL << (asid % RISCV64_ASID_WORD_BITS);
            uint32_t word = asid / RISCV64_ASID_WORD_BITS;
            if (!(g_riscv64_asid_eligible[word] & bit))
                continue;
            g_riscv64_asid_eligible[word] &= ~bit;
            g_riscv64_asid_live[word] |= bit;
            mutex_unlock(&g_riscv64_asid_lock);
            return asid;
        }

        /* Only ASIDs which were already free at this global-flush boundary
         * become eligible. An ASID freed later remains quarantined until the
         * next boundary, so stale translations can never alias a new mm. */
        arch_tlb_flush();
        for (uint32_t word = 0; word < RISCV64_ASID_WORDS; word++)
            g_riscv64_asid_eligible[word] = ~g_riscv64_asid_live[word];
        g_riscv64_asid_eligible[0] &= ~1UL;
        if (max_asid != 0xffffU) {
            for (uint32_t asid = max_asid + 1; asid < RISCV64_ASID_COUNT;
                 asid++)
                g_riscv64_asid_eligible[asid / RISCV64_ASID_WORD_BITS] &=
                    ~(1UL << (asid % RISCV64_ASID_WORD_BITS));
        }
    }

    mutex_unlock(&g_riscv64_asid_lock);
    panic("RISC-V ASID space exhausted (%u concurrent address spaces)",
          max_asid);
}

void riscv64_asid_release(uint32_t asid)
{
    if (!asid)
        return;
    mutex_lock(&g_riscv64_asid_lock);
    uint32_t word = asid / RISCV64_ASID_WORD_BITS;
    uint64_t bit = 1UL << (asid % RISCV64_ASID_WORD_BITS);
    if (!(g_riscv64_asid_live[word] & bit)) {
        mutex_unlock(&g_riscv64_asid_lock);
        panic("RISC-V ASID double release: %u", asid);
    }
    g_riscv64_asid_live[word] &= ~bit;
    mutex_unlock(&g_riscv64_asid_lock);
}
