#ifdef CONFIG_RISCV64

#include "core/types.h"
#include "core/consts.h"
#include "core/string.h"
#include "core/stdio.h"
#include "firmware.h"
#include "platform.h"

#define FDT_MAGIC       0xd00dfeedU
#define FDT_BEGIN_NODE  1U
#define FDT_END_NODE    2U
#define FDT_PROP        3U
#define FDT_NOP         4U
#define FDT_END         9U

static uint32_t be32(uint32_t v)
{
    return ((v >> 24) & 0xffU) |
           ((v >>  8) & 0xff00U) |
           ((v <<  8) & 0xff0000U) |
           ((v << 24) & 0xff000000U);
}

static uint32_t read_be32(const void *p)
{
    return be32(*(const uint32_t *)p);
}

static uint64_t read_be64(const void *p)
{
    return ((uint64_t)read_be32(p) << 32) |
           (uint64_t)read_be32((const uint8_t *)p + 4);
}

static const char *skip_name(const char *p)
{
    while (*p)
        p++;
    return p + 1;
}

extern uint64_t __boot_dtb_ptr;

static paddr_t riscv64_ram_base = PHYS_MEMORY_BASE;
static paddr_t riscv64_ram_end = PHYS_MEMORY_END;

size_t arch_ram_range_count(void)
{
    return 1;
}

int arch_ram_range(size_t idx, paddr_t *base, paddr_t *end)
{
    if (idx != 0 || !base || !end)
        return -1;
    *base = riscv64_ram_base;
    *end = riscv64_ram_end;
    return 0;
}

void riscv64_memory_init(void)
{
    const uint8_t *base = (const uint8_t *)(uintptr_t)__boot_dtb_ptr;
    if (!base || read_be32(base) != FDT_MAGIC) {
        printf("[FDT] memory node unavailable, using 1 GiB fallback\n");
        return;
    }

    uint32_t totalsize = read_be32(base + 4);
    uint32_t off_struct = read_be32(base + 8);
    uint32_t off_strings = read_be32(base + 12);
    if (totalsize < 40 || off_struct >= totalsize || off_strings >= totalsize) {
        printf("[FDT] malformed header, using 1 GiB fallback\n");
        return;
    }

    const uint8_t *p = base + off_struct;
    const uint8_t *endp = base + totalsize;
    const uint8_t *stringsp = base + off_strings;
    int depth = 0;
    int memory_depth = -1;

    while (p + 4 <= endp) {
        uint32_t token = read_be32(p);
        p += 4;
        if (token == FDT_END)
            break;

        if (token == FDT_BEGIN_NODE) {
            const char *name = (const char *)p;
            const char *name_end = name;
            while ((const uint8_t *)name_end < endp && *name_end)
                name_end++;
            if ((const uint8_t *)name_end >= endp)
                break;
            depth++;
            if (depth == 2 &&
                (strcmp(name, "memory") == 0 ||
                 strncmp(name, "memory@", 7) == 0))
                memory_depth = depth;
            p = (const uint8_t *)(((uintptr_t)(name_end + 1) + 3) & ~3UL);
            continue;
        }

        if (token == FDT_END_NODE) {
            if (depth == memory_depth)
                memory_depth = -1;
            depth--;
            continue;
        }

        if (token == FDT_NOP)
            continue;

        if (token != FDT_PROP || p + 8 > endp)
            break;

        uint32_t len = read_be32(p);
        uint32_t nameoff = read_be32(p + 4);
        p += 8;
        if (p + ((len + 3U) & ~3U) > endp ||
            off_strings + nameoff >= totalsize)
            break;

        const char *propname = (const char *)(stringsp + nameoff);
        if (depth == memory_depth && strcmp(propname, "reg") == 0 && len >= 16) {
            uint64_t ram_base = read_be64(p);
            uint64_t ram_size = read_be64(p + 8);
            uint64_t ram_end = ram_base + ram_size;
            if (ram_end < ram_base)
                break;
            if (ram_base <= PHYS_MEMORY_BASE && ram_end > KERNEL_ENTRY) {
                if (ram_end > PHYS_MEMORY_MAX_END) {
                    printf("[FDT] RAM end 0x%lx exceeds boot map, clamping to 0x%lx\n",
                           (unsigned long)ram_end,
                           (unsigned long)PHYS_MEMORY_MAX_END);
                    ram_end = PHYS_MEMORY_MAX_END;
                }
                riscv64_ram_base = PHYS_MEMORY_BASE;
                riscv64_ram_end = (paddr_t)(ram_end & ~(PAGE_SIZE - 1));
                printf("[FDT] RAM range 0x%lx..0x%lx (%lu MiB)\n",
                       (unsigned long)riscv64_ram_base,
                       (unsigned long)riscv64_ram_end,
                       (unsigned long)((riscv64_ram_end -
                                        riscv64_ram_base) >> 20));
                return;
            }
        }
        p += (len + 3U) & ~3U;
    }

    printf("[FDT] memory node parse failed, using 1 GiB fallback\n");
}

static int fdt_extract_bootargs(uint64_t dtb_pa, char *out, size_t outsz)
{
    if (!out || outsz == 0)
        return -1;
    out[0] = '\0';

    if (dtb_pa == 0)
        return -1;

    const uint8_t *base = (const uint8_t *)dtb_pa;
    if (read_be32(base) != FDT_MAGIC)
        return -1;

    uint32_t totalsize    = read_be32(base + 4);
    uint32_t off_struct   = read_be32(base + 8);
    uint32_t off_strings  = read_be32(base + 12);
    (void)totalsize;

    const uint8_t *structp  = base + off_struct;
    const uint8_t *stringsp = base + off_strings;
    const uint8_t *p = structp;

    int depth = 0;
    int in_chosen = 0;

    for (;;) {
        uint32_t token = read_be32(p);
        p += 4;

        if (token == FDT_END)
            break;

        switch (token) {
        case FDT_BEGIN_NODE: {
            const char *name = (const char *)p;
            p = (const uint8_t *)skip_name(name);
            /* Align to 4 bytes */
            p = (const uint8_t *)(((uintptr_t)p + 3) & ~3UL);
            depth++;
            if (depth == 2 && strcmp(name, "chosen") == 0)
                in_chosen = 1;
            break;
        }
        case FDT_END_NODE:
            if (depth == 2 && in_chosen)
                in_chosen = 0;
            depth--;
            break;
        case FDT_PROP: {
            uint32_t len     = read_be32(p);
            uint32_t nameoff = read_be32(p + 4);
            p += 8;
            const char *propname = (const char *)(stringsp + nameoff);
            if (in_chosen && strcmp(propname, "bootargs") == 0) {
                size_t n = len;
                if (n > outsz - 1)
                    n = outsz - 1;
                memcpy(out, p, n);
                out[n] = '\0';
                return 0;
            }
            p += ((len + 3) & ~3U);
            break;
        }
        case FDT_NOP:
            break;
        default:
            return -1;
        }

        if (depth < 0)
            break;
    }

    return -1;
}

const char *arch_bootargs_get(void)
{
    static char bootargs_buf[1024];
    static int ready;

    if (!ready) {
        bootargs_buf[0] = '\0';
        uint64_t dtb = __boot_dtb_ptr;
        printf("[FDT] dtb_ptr=0x%lx\n", dtb);
        if (dtb && fdt_extract_bootargs(dtb, bootargs_buf, sizeof(bootargs_buf)) == 0) {
            printf("[FDT] bootargs='%s'\n", bootargs_buf);
        } else {
            printf("[FDT] no bootargs extracted\n");
        }
        ready = 1;
    }
    return bootargs_buf[0] ? bootargs_buf : NULL;
}

#endif /* CONFIG_RISCV64 */
