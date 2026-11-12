#ifdef CONFIG_X86_64
#include "core/types.h"
#include "firmware.h"
#include "firmware/acpi.h"
#include "cpu.h"
#include "console.h"
#include "platform.h"
#include "core/string.h"
#include "core/klog.h"
#include "mm/frame.h"
#include "core/stdio.h"
#include "core/panic.h"
#include "core/errno.h"
#include "boot_memory.h"
#include "boot/framebuffer.h"
extern int x86_boot_framebuffer_get(boot_framebuffer_t *);

extern uint64_t x86_boot_acpi_rsdp;
extern uint64_t x86_booted_uefi;
static uint64_t g_firmware_rsdp_pa;
static acpi_context_t x86_acpi;
static int x86_acpi_state;
static const void *x86_acpi_map(void *opaque, uint64_t pa, size_t size) {
    (void)opaque;
    /* This is a bootstrap direct-map policy, not an ACPI table assumption. */
    if (!size || pa >= 0x100000000ULL || size > 0x100000000ULL - pa)
        return NULL;
    return (const void *)(uintptr_t)(PAGE_OFFSET + pa);
}
static void x86_acpi_unmap(void *opaque, const void *ptr, size_t size) {
    (void)opaque; (void)ptr; (void)size;
}
extern void *arch_device_map_mmio(uint64_t, size_t, uint32_t);
static const void *x86_acpi_region(void *opaque, uint64_t pa, size_t size) {
    (void)opaque;
    return arch_device_map_mmio(pa, size, 0);
}
static const acpi_mapper_t x86_acpi_mapper = {
    .map = x86_acpi_map, .unmap = x86_acpi_unmap, .map_region = x86_acpi_region,
};
void firmware_set_rsdp_pa(uintptr_t pa) {
    g_firmware_rsdp_pa = pa;
    x86_acpi_state = 0;
}
static uint64_t x86_rsdp_scan(uint64_t first, uint64_t end) {
    for (uint64_t pa = (first + 15) & ~15ULL; pa < end && end - pa >= 20; pa += 16) {
        const void *p = x86_acpi_map(NULL, pa, 20);
        acpi_context_t c;
        if (p && !memcmp(p, "RSD PTR ", 8) &&
            acpi_context_open(&c, &x86_acpi_mapper, pa) == ACPI_OK)
            return pa;
    }
    return 0;
}
uint64_t arch_firmware_rsdp_pa(void) {
    if (x86_boot_acpi_rsdp)
        return x86_boot_acpi_rsdp;
    if (g_firmware_rsdp_pa)
        return g_firmware_rsdp_pa;
    const uint8_t *bda = x86_acpi_map(NULL, 0x40e, 2);
    uint64_t ebda = bda ? (uint64_t)acpi_le16(bda) << 4 : 0;
    uint64_t pa = ebda >= 0x400 && ebda < 0xa0000 ? x86_rsdp_scan(ebda, ebda + 1024) : 0;
    return pa ? pa : x86_rsdp_scan(0xe0000, 0x100000);
}
const acpi_context_t *firmware_acpi_context(void) {
    if (!x86_acpi_state) {
        x86_acpi_state = acpi_context_open(&x86_acpi, &x86_acpi_mapper,
                                          arch_firmware_rsdp_pa()) == 0 ? 1 : -1;
    }
    return x86_acpi_state == 1 ? &x86_acpi : NULL;
}
static int x86_find_table(const char sig[4], acpi_table_t *t) {
    const acpi_context_t *c = firmware_acpi_context();
    return c ? acpi_table_find(c, sig, 0, t) : ACPI_NOT_FOUND;
}
static void x86_release(acpi_table_t *t) {
    acpi_table_release(&x86_acpi, t);
}
size_t firmware_acpi_apic_ids(uint32_t *ids, size_t capacity, uint32_t bsp) {
    if (!ids || !capacity)
        return 0;
    acpi_table_t table;
    acpi_madt_iterator_t it;
    if (x86_find_table("APIC", &table))
        return 0;
    if (acpi_madt_begin(&table, &it)) {
        x86_release(&table);
        return 0;
    }
    ids[0] = bsp;
    size_t count = 1;
    int found_bsp = 0;
    acpi_madt_entry_t e;
    while (acpi_madt_next(&it, &e) > 0) {
        uint32_t id, flags;
        if (acpi_madt_cpu(&e, &id, &flags) || !(flags & 1U) || id > 255)
            continue; /* Online-capable alone does not mean enabled. */
        if (id == bsp) {
            found_bsp = 1;
            continue;
        }
        int duplicate = 0;
        for (size_t i = 0; i < count; i++)
            duplicate |= ids[i] == id;
        if (!duplicate && count < capacity)
            ids[count++] = id;
    }
    x86_release(&table);
    return found_bsp ? count : 0;
}
const char *firmware_bios_or_uefi(void) {
    return x86_booted_uefi ? "UEFI" : "Multiboot";
}
static int x86_mcfg(acpi_mcfg_entry_t *out) {
    acpi_table_t table;
    if (x86_find_table("MCFG", &table))
        return -1;
    size_t count;
    int rc = -1;
    if (!acpi_mcfg_count(&table, &count)) {
        for (size_t i = 0; i < count; i++) {
            acpi_mcfg_entry_t e;
            if (!acpi_mcfg_get(&table, i, &e) && !e.segment &&
                e.base <= 0x100000000ULL - (((uint64_t)e.end_bus + 1) << 20)) {
                *out = e;
                rc = 0;
                break;
            }
        }
    }
    x86_release(&table);
    return rc;
}
uintptr_t firmware_acpi_mcfg_base(void) {
    acpi_mcfg_entry_t e;
    return x86_mcfg(&e) ? 0 : (uintptr_t)(PAGE_OFFSET + e.base);
}
int firmware_acpi_mcfg_bus_range(uint8_t *start, uint8_t *end) {
    acpi_mcfg_entry_t e;
    if (!start || !end || x86_mcfg(&e))
        return -1;
    *start = e.start_bus; *end = e.end_bus;
    return 0;
}
static uint64_t acpi_field(const acpi_table_t *table, size_t off, unsigned width) {
    uint64_t n = 0;
    acpi_table_read(table, off, width, &n);
    return n;
}
uintptr_t firmware_acpi_hpet_address(void) {
    acpi_table_t table;
    if (x86_find_table("HPET", &table))
        return 0;
    uint64_t address = acpi_field(&table, 44, 8);
    int valid = table.length >= 56 && table.data[40] == 0 && address &&
                address <= 0xffffffffULL - 0x400;
    x86_release(&table);
    return valid ? (uintptr_t)(PAGE_OFFSET + address) : 0;
}
size_t firmware_acpi_ioapics(firmware_acpi_ioapic_t *out, size_t max) {
    acpi_table_t table;
    acpi_madt_iterator_t it;
    if (x86_find_table("APIC", &table))
        return 0;
    if (acpi_madt_begin(&table, &it)) {
        x86_release(&table);
        return 0;
    }
    size_t count = 0;
    acpi_madt_entry_t e;
    while (acpi_madt_next(&it, &e) > 0) {
        uint32_t id, pa, base;
        if (acpi_madt_ioapic(&e, &id, &pa, &base))
            continue;
        if (out && count < max)
            out[count] = (firmware_acpi_ioapic_t){id, pa, base};
        count++;
    }
    x86_release(&table);
    return count;
}
void firmware_acpi_poweroff(void) {
    fw_node_t root;
    const acpi_context_t *context = firmware_acpi_context();
    if (context && (!acpi_namespace_root(&root) ||
                    !acpi_namespace_initialize(context, &root)))
        (void)acpi_namespace_poweroff();
    /* No guessed SLP_TYP or emulator-specific register writes on real PCs. */
    arch_halt();
}

void firmware_shutdown(void) {
    firmware_acpi_poweroff();
}

/* QEMU fw_cfg (port 0x510 selector / 0x511 data).
 *
 * The command line is *not* reachable through the old fixed keys
 * FW_CFG_CMDLINE_SIZE/DATA (0x14/0x15): those belong to the Linux boot
 * protocol and QEMU only fills them in when it boots a bzImage through a
 * setup header.  Under `-kernel` multiboot they read back as zero, which is
 * why bootargs used to come out empty here and every a20.* knob -- static
 * network config, trace=<comm> -- was unreachable on x86_64.
 *
 * What QEMU actually publishes is the file directory: every `-fw_cfg
 * name=...,file/string=...` item gets a selector and a 64-byte record in a
 * flat table at FW_CFG_FILE_DIR, so `-fw_cfg name=opt/x86/cmdline,string=...`
 * is a supported way to hand the kernel a command line.  Read the directory,
 * find the name, then read the file through the selector its record names. */
#define FW_CFG_SELECTOR_PORT 0x510
#define FW_CFG_DATA_PORT     0x511
#define FW_CFG_SIGNATURE     0x0000
#define FW_CFG_FILE_DIR      0x0019
#define FW_CFG_FILE_NAME_LEN 56
#define FW_CFG_FILE_REC_LEN  64
#define FW_CFG_FILE_MAX      256
#define FW_CFG_CMDLINE_FILE  "opt/x86/cmdline"

static uint8_t fw_cfg_read8(void) {
    return inb(FW_CFG_DATA_PORT);
}

static uint32_t fw_cfg_read32(void) {
    uint32_t value = 0;
    for (int i = 0; i < 4; i++)
        value = (value << 8) | fw_cfg_read8();
    return value;
}

/* Everything in fw_cfg is big-endian and the data port is one sequential
 * stream per selected item: writing the selector rewinds to that item, and
 * each following read just keeps walking forward. */
static uint16_t fw_cfg_read16(void) {
    return (uint16_t)((fw_cfg_read8() << 8) | fw_cfg_read8());
}

/* Copy the fw_cfg file called `name` into `out`.  The directory is a
 * big-endian entry count followed by that many fixed 64-byte records of
 * {u32 size, u16 select, u16 reserved, char name[56]}; a file's bytes are
 * then read straight from the selector its record carries, with no length
 * prefix.  Returns 0 on success. */
static int fw_cfg_read_file(const char *name, char *out, size_t outsz) {
    outw(FW_CFG_SELECTOR_PORT, FW_CFG_FILE_DIR);
    uint32_t count = fw_cfg_read32();
    /* No directory at all, or a count no real machine would publish: both mean
     * there is nothing named `name`, and guessing past that would feed
     * whatever the port happens to return to every bootargs consumer. */
    if (count == 0 || count > FW_CFG_FILE_MAX)
        return -1;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t size = fw_cfg_read32();
        uint16_t select = fw_cfg_read16();
        (void)fw_cfg_read16();        /* reserved */
        char entry[FW_CFG_FILE_NAME_LEN];
        for (int n = 0; n < FW_CFG_FILE_NAME_LEN; n++)
            entry[n] = (char)fw_cfg_read8();
        entry[FW_CFG_FILE_NAME_LEN - 1] = '\0';

        if (strcmp(entry, name) != 0)
            continue;
        if (size >= outsz)
            size = outsz - 1;
        outw(FW_CFG_SELECTOR_PORT, select);
        for (uint32_t k = 0; k < size; k++)
            out[k] = (char)fw_cfg_read8();
        out[size] = '\0';
        return 0;
    }
    return -1;
}

/* Copied by both boot entry paths before clearing BSS. QEMU may place the
 * Multiboot string directly after the loaded image, inside our BSS range. */
__attribute__((section(".data"))) char g_bootargs[256];

#define MULTIBOOT_TAG_ACPI_OLD 14u

/* Defined further down with the rest of the multiboot state. */
extern __attribute__((section(".data"))) volatile uint32_t g_mb_magic;
extern __attribute__((section(".data"))) volatile uint32_t g_mb_info;

static int firmware_bootcount_only(const char *args) {
    while (*args == ' ' || *args == '\t') args++;
    if (strncmp(args, "a20.bootcount=", 14)) return 0;
    args += 14;
    if (*args < '0' || *args > '9') return 0;
    while (*args >= '0' && *args <= '9') args++;
    while (*args == ' ' || *args == '\t') args++;
    return !*args;
}

static int fw_cfg_is_qemu(void) {
    uint32_t a, b, c, d;
    __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                         : "a"(0x40000000U), "c"(0));
    char vendor[12];
    memcpy(vendor, &b, 4); memcpy(vendor + 4, &c, 4); memcpy(vendor + 8, &d, 4);
    return !memcmp(vendor, "KVMKVMKVM", 9) || !memcmp(vendor, "TCGTCGTCGTCG", 12);
}

static void fw_cfg_append_loader_options(void) {
    /* The EFI loader always appends its boot counter. Metadata alone must
     * not hide an explicit emulator command line. Genuine EFI LoadOptions
     * retain precedence; never probe this I/O device on a physical machine. */
    if (!firmware_bootcount_only(g_bootargs) || !fw_cfg_is_qemu())
        return;
    char extra[sizeof(g_bootargs)];
    outw(FW_CFG_SELECTOR_PORT, FW_CFG_SIGNATURE);
    if (fw_cfg_read32() != 0x51454d55U ||
        fw_cfg_read_file(FW_CFG_CMDLINE_FILE, extra, sizeof(extra)))
        return;
    size_t before = strlen(g_bootargs), addition = strlen(extra);
    if (addition && before + addition + 2 <= sizeof(g_bootargs)) {
        g_bootargs[before++] = ' ';
        memcpy(g_bootargs + before, extra, addition + 1);
    }
}

const char *firmware_bootargs(void) {
    static int loader_options_probed;
    if (!loader_options_probed) {
        loader_options_probed = 1;
        fw_cfg_append_loader_options();
    }
    static int mb_probed;
    if (!mb_probed) {
        mb_probed = 1;
        /* Boot assembly already copied Multiboot text into kernel-owned data.
         * Report it once; fw_cfg remains the fallback when no text was given. */
        if (g_bootargs[0]) {
            printf("[BOOTARGS] multiboot cmdline='%s'\n", g_bootargs);
        }
    }
    if (g_bootargs[0])
        return g_bootargs;

    static int fw_probed;
    if (!fw_probed) {
        fw_probed = 1;
        outw(FW_CFG_SELECTOR_PORT, FW_CFG_SIGNATURE);
        uint32_t sig = fw_cfg_read32();
        printf("[FW_CFG] signature=0x%08x\n", sig);
        if (sig == 0x51454d55U &&   /* "QEMU", big-endian */
            fw_cfg_read_file(FW_CFG_CMDLINE_FILE, g_bootargs, sizeof(g_bootargs)) == 0)
            printf("[FW_CFG] %s='%s'\n", FW_CFG_CMDLINE_FILE, g_bootargs);
        else
            printf("[FW_CFG] no %s, using fallback bootargs\n", FW_CFG_CMDLINE_FILE);
    }
    return g_bootargs;
}

void firmware_reboot(void) {
    uint8_t val;
    do {
        val = inb(0x64);
    } while (val & 0x02);
    outb(0x64, 0xFE);
    arch_halt();
}

void firmware_set_timer(uint64_t time) {
    (void)time;
}

void firmware_console_putchar(char c) {
    arch_uart_putc(c);
}

int firmware_console_getchar(void) {
    return arch_uart_poll_getc();
}

/* The bootstrap maps 0..4 GiB independently of firmware input. RAM above
 * that window is admitted only as wholly usable 1 GiB chunks, backed by
 * ordinary 2 MiB leaves (no optional 1 GiB-page CPU requirement). */
#define X86_HIGH_RAM_BASE 0x100000000ULL
#define X86_HIGH_PAGE 0x40000000ULL
extern uint64_t boot_pdpt_hh[512];
static uint64_t high_ram_pd[(X86_HIGH_RAM_MAP_END-X86_HIGH_RAM_BASE)>>30][512]
    __attribute__((aligned(4096)));
static void x86_high_ram_map_flush(void) {
    __asm__ __volatile__("movq %%cr3, %%rax\n\tmovq %%rax, %%cr3\n\t" ::: "rax", "memory");
}
__attribute__((section(".data"))) volatile uint32_t g_mb_magic;
__attribute__((section(".data"))) volatile uint32_t g_mb_info;
static paddr_t g_ram_base[PFA_MAX_RANGES], g_ram_end[PFA_MAX_RANGES];
static size_t g_ram_count;
static int g_ram_done;
static x86_boot_range_t acpi_exclusions[ACPI_MAX_ROOT_ENTRIES + 8];
static size_t acpi_exclusion_count;
static int x86_reserve_acpi(void *opaque,uint64_t physical,size_t length) {
    (void)opaque;
    if (!length || length > (uint64_t)-1-physical) return -EOVERFLOW;
    uint64_t end=physical+length;
    if (end > (uint64_t)-1-4095) return -EOVERFLOW;
    physical&=~4095ULL;end=(end+4095)&~4095ULL;
    /* Allocation is page based. Coalesce packed/duplicate table pages before
     * applying the bounded early-storage capacity. */
    for (size_t i=0;i<acpi_exclusion_count;) {
        x86_boot_range_t *range=&acpi_exclusions[i];
        if (physical<=range->end && range->base<=end) {
            if (range->base<physical)physical=range->base;
            if (range->end>end)end=range->end;
            acpi_exclusions[i]=acpi_exclusions[--acpi_exclusion_count];i=0;
        } else i++;
    }
    if (acpi_exclusion_count==sizeof(acpi_exclusions)/sizeof(acpi_exclusions[0])) return -ENOSPC;
    acpi_exclusions[acpi_exclusion_count++]=(x86_boot_range_t){physical,end};return 0;
}
static const void *x86_boot_memory_read(void *context,uint64_t pa,size_t size) {
    (void)context;
    if (!size || pa>=0x100000000ULL || size>0x100000000ULL-pa) return NULL;
    return (const void *)(uintptr_t)(PAGE_OFFSET+pa);
}
static void x86_ram_detect(void) {
    x86_boot_range_t ranges[PFA_MAX_RANGES];
    enum x86_boot_memory_source source;
    size_t count=0;
    const acpi_context_t *acpi=firmware_acpi_context();
    acpi_exclusion_count=0;
    boot_framebuffer_t scanout;
    if (x86_boot_framebuffer_get(&scanout) < 0)
        panic("x86: invalid framebuffer handoff");
    if (scanout.magic &&
        x86_reserve_acpi(NULL, scanout.physical, scanout.aperture_bytes))
        panic("x86: framebuffer reservation failed");
    if (acpi && acpi_reserve_tables(acpi,x86_reserve_acpi,NULL))
        panic("x86: invalid or excessive ACPI table reservations");
    int rc=x86_boot_memory_decode(g_mb_magic,g_mb_info,x86_boot_memory_read,NULL,
        PHYS_MEMORY_END,X86_HIGH_RAM_MAP_END,acpi_exclusions,acpi_exclusion_count,
        ranges,PFA_MAX_RANGES,&count,&source);
    if(rc) { printf("[RAM] decode failed rc=%d magic=0x%x info=0x%x\n", rc, g_mb_magic, g_mb_info); panic("x86: malformed or excessive boot memory map"); }
    /* PFA places metadata after _bss_end and excludes the kernel prefix in
     * this range. A reserved hole through the kernel must never split it. */
    extern char _bss_end[];
    uint64_t kernel_end=(uintptr_t)_bss_end-PAGE_OFFSET;
    int kernel_found=0,mapped_high=0;
    for(size_t i=0;i<count;i++)
        if(ranges[i].base<=KERNEL_ENTRY && ranges[i].end>=kernel_end) kernel_found=1;
    if(!kernel_found) panic("x86: linked kernel is outside usable boot RAM");
    for(size_t i=0;i<count;i++) {
        g_ram_base[i]=ranges[i].base;g_ram_end[i]=ranges[i].end;
        if(ranges[i].base<X86_HIGH_RAM_BASE) continue;
        for(uint64_t chunk=ranges[i].base;chunk<ranges[i].end;chunk+=X86_HIGH_PAGE) {
            uint64_t *pd=high_ram_pd[(chunk-X86_HIGH_RAM_BASE)>>30];
            for(unsigned j=0;j<512;j++) pd[j]=(chunk+(uint64_t)j*0x200000)|0x83ULL;
            boot_pdpt_hh[chunk>>30]=((uintptr_t)pd-PAGE_OFFSET)|0x003ULL;
            mapped_high=1;
        }
    }
    g_ram_count=count;g_ram_done=1;
    if(source==X86_BOOT_MEMORY_FIXED)
        kinfo("[RAM] boot memory map absent; using board RAM fallback\n");
    for(size_t i=0;i<count;i++)
        printf("[RAM] usable %p..%p (%lu MiB)\n",(void *)g_ram_base[i],(void *)g_ram_end[i],
            (unsigned long)((g_ram_end[i]-g_ram_base[i])>>20));
    if(mapped_high)x86_high_ram_map_flush();
}

size_t arch_ram_range_count(void) {
    if (!g_ram_done)
        x86_ram_detect();
    return g_ram_count;
}

int arch_ram_range(size_t idx, paddr_t *base, paddr_t *end) {
    if (!g_ram_done)
        x86_ram_detect();
    if (idx >= g_ram_count || !base || !end)
        return -1;
    *base = g_ram_base[idx];
    *end = g_ram_end[idx];
    return 0;
}

#endif
