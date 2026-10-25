#ifdef CONFIG_RISCV64

#include "core/types.h"
#include "core/consts.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/bootargs.h"
#include "firmware.h"
#include "platform.h"
#include "fdt_isa.h"
#include "fdt_memory.h"
#include "core/panic.h"
#include "drivers/core/driver_core.h"

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

extern uint64_t __boot_dtb_ptr;

static paddr_t riscv64_ram_base = PHYS_MEMORY_BASE;
static paddr_t riscv64_ram_end = PHYS_MEMORY_END;
/* OpenSBI's DTB pointer is a temporary physical address.  Snapshot the ISA
 * facts needed after MMU setup while the boot identity mapping is alive. */
static int riscv64_fdt_isa_cache_ready;
static int riscv64_fdt_has_h;
static int riscv64_fdt_has_sstc;

/* The selected board declares the physical window the kernel may use.  The
 * FDT memory node then narrows that window to the RAM actually populated, so
 * the same memory discovery works for QEMU virt (0x80000000..) and for the
 * StarFive VisionFive 2 (0x40000000..) without hard-coding one board here. */
static void riscv64_window_from_board(void)
{
    if (current_board &&
        current_board->ram_end > current_board->ram_base) {
        riscv64_ram_base = current_board->ram_base;
        riscv64_ram_end = current_board->ram_end;
    }
}

int riscv64_fdt_has_isa_extension(const char *extension)
{
    if (!extension)
        return 0;
    if (riscv64_fdt_isa_cache_ready) {
        if (strcmp(extension, "h") == 0)
            return riscv64_fdt_has_h;
        if (strcmp(extension, "sstc") == 0)
            return riscv64_fdt_has_sstc;
        /* Do not dereference the firmware's temporary buffer after the
         * boot mapping is gone.  These are the only late-query consumers. */
        return 0;
    }

    /* Before MMU initialization, OpenSBI's physical pointer is reachable
     * through the temporary identity map. */
    const uint8_t *base = (const uint8_t *)(uintptr_t)__boot_dtb_ptr;
    if (!base || !extension || read_be32(base) != FDT_MAGIC)
        return 0;

    uint32_t totalsize = read_be32(base + 4);
    uint32_t off_struct = read_be32(base + 8);
    uint32_t off_strings = read_be32(base + 12);
    if (totalsize < 40 || off_struct >= totalsize || off_strings >= totalsize)
        return 0;

    const uint8_t *p = base + off_struct;
    const uint8_t *endp = base + totalsize;
    const uint8_t *stringsp = base + off_strings;
    while (p + 4 <= endp) {
        uint32_t token = read_be32(p);
        p += 4;
        if (token == FDT_END)
            break;
        if (token == FDT_BEGIN_NODE) {
            const char *name_end = (const char *)p;
            while ((const uint8_t *)name_end < endp && *name_end)
                name_end++;
            if ((const uint8_t *)name_end >= endp)
                return 0;
            p = (const uint8_t *)(((uintptr_t)(name_end + 1) + 3) & ~3UL);
            continue;
        }
        if (token == FDT_END_NODE || token == FDT_NOP)
            continue;
        if (token != FDT_PROP || p + 8 > endp)
            return 0;

        uint32_t len = read_be32(p);
        uint32_t nameoff = read_be32(p + 4);
        p += 8;
        uint32_t padded = (len + 3U) & ~3U;
        if (p + padded > endp || off_strings + nameoff >= totalsize)
            return 0;
        const char *propname = (const char *)(stringsp + nameoff);
        int legacy_isa = strcmp(propname, "riscv,isa") == 0;
        if ((legacy_isa || strcmp(propname, "riscv,isa-extensions") == 0) &&
            fdt_isa_string_has_extension((const char *)p, len, extension,
                                         legacy_isa))
            return 1;
        p += padded;
    }
    return 0;
}

/* Read the RISC-V timer timebase-frequency from the DTB (/cpus).  Returns 0
 * when no usable value is published, so boards can fall back to a known
 * constant (QEMU virt = 10 MHz, StarFive JH7110 = 24 MHz). */
uint64_t riscv64_fdt_timebase_freq(void)
{
    const uint8_t *base = (const uint8_t *)(uintptr_t)__boot_dtb_ptr;
    if (!base || read_be32(base) != FDT_MAGIC)
        return 0;

    uint32_t totalsize = read_be32(base + 4);
    uint32_t off_struct = read_be32(base + 8);
    uint32_t off_strings = read_be32(base + 12);
    if (totalsize < 40 || off_struct >= totalsize || off_strings >= totalsize)
        return 0;

    const uint8_t *p = base + off_struct;
    const uint8_t *endp = base + totalsize;
    const uint8_t *stringsp = base + off_strings;
    while (p + 4 <= endp) {
        uint32_t token = read_be32(p);
        p += 4;
        if (token == FDT_END)
            break;
        if (token == FDT_BEGIN_NODE) {
            const char *name = (const char *)p;
            while ((const uint8_t *)name < endp && *name)
                name++;
            if ((const uint8_t *)name >= endp)
                return 0;
            p = (const uint8_t *)(((uintptr_t)(name + 1) + 3) & ~3UL);
            continue;
        }
        if (token == FDT_END_NODE || token == FDT_NOP)
            continue;
        if (token != FDT_PROP || p + 8 > endp)
            return 0;

        uint32_t len = read_be32(p);
        uint32_t nameoff = read_be32(p + 4);
        p += 8;
        uint32_t padded = (len + 3U) & ~3U;
        if (p + padded > endp || off_strings + nameoff >= totalsize)
            return 0;
        const char *propname = (const char *)(stringsp + nameoff);
        if (strcmp(propname, "timebase-frequency") == 0 && len >= 4)
            return read_be32(p);
        p += padded;
    }
    return 0;
}

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
    riscv64_window_from_board();
    extern char _start[], _end[];
    uint64_t dtb = __boot_dtb_ptr;
    size_t available = 0;
    if (dtb) {
        if ((dtb & 7) || dtb < riscv64_ram_base || dtb >= riscv64_ram_end)
            panic("RISC-V: firmware DTB lies outside the board boot RAM map");
        uint64_t mapped = riscv64_ram_end - dtb;
        available = mapped < 0x200000 ? (size_t)mapped : 0x200000;
    }
    riscv_fdt_memory_t memory;
    int rc = riscv_fdt_memory_window((const void *)(uintptr_t)dtb, available,
        dtb, riscv64_ram_base, riscv64_ram_end,
        (uintptr_t)_start - PAGE_OFFSET, (uintptr_t)_end - PAGE_OFFSET, &memory);
    if (rc < 0)
        panic("RISC-V: invalid FDT RAM/reservation or firmware overlaps kernel");
    riscv64_ram_base = (paddr_t)memory.base;
    riscv64_ram_end = (paddr_t)memory.end;
    if (rc == 1) {
        printf("[FDT] memory node unavailable, using board window 0x%lx..0x%lx\n",
               (unsigned long)riscv64_ram_base, (unsigned long)riscv64_ram_end);
    } else {
        printf("[FDT] RAM range 0x%lx..0x%lx (%lu MiB, firmware reservations excluded)\n",
               (unsigned long)riscv64_ram_base, (unsigned long)riscv64_ram_end,
               (unsigned long)((riscv64_ram_end - riscv64_ram_base) >> 20));
    }
    /* Snapshot ISA facts only after validating the DTB. Physical boot mappings
     * are removed later; the firmware buffer is now excluded from allocation. */
    if (!riscv64_fdt_isa_cache_ready) {
        riscv64_fdt_has_h = riscv64_fdt_has_isa_extension("h");
        riscv64_fdt_has_sstc = riscv64_fdt_has_isa_extension("sstc");
        riscv64_fdt_isa_cache_ready = 1;
    }
}

const char *arch_bootargs_get(void)
{
    static char bootargs_buf[1024];
    static int ready;

    if (!ready) {
        bootargs_buf[0] = '\0';
        uint64_t dtb = __boot_dtb_ptr;
        printf("[FDT] dtb_ptr=0x%lx\n", dtb);
        if (dtb && fdt_extract_bootargs((const void *)(uintptr_t)dtb, bootargs_buf, sizeof(bootargs_buf)) == 0) {
            printf("[FDT] bootargs='%s'\n", bootargs_buf);
        } else {
            printf("[FDT] no bootargs extracted\n");
        }
        ready = 1;
    }
    if (bootargs_buf[0])
        return bootargs_buf;
#ifdef CONFIG_BOARD_VISIONFIVE2
    /* The direct SPL -> OpenSBI -> A20OS FIT path does not synthesize a
     * /chosen/bootargs property.  DHCP lets a normal physical LAN provide
     * the address, gateway and DNS without baking a site-specific subnet into
     * the image.  DNS is a fallback until the DHCP lease is synchronized. */
    return "a20.dhcp=1 a20.dns=8.8.8.8 a20.hostname=a20os-vf2";
#else
    return NULL;
#endif
}

#endif /* CONFIG_RISCV64 */
