/*
 * x86_64 UEFI loader for the VirtualBox board.
 *
 * Why not GRUB: GRUB 2.12 does not fill the multiboot ACPI tags, so a multiboot
 * kernel booted by GRUB under UEFI never learns the RSDP address.  Without it
 * there is no MCFG, PCI falls back to a window holding no devices, no block
 * driver binds, and the boot stops at "init: no init program found".  The
 * firmware publishes the RSDP in its configuration table, so read it from there.
 *
 * Why there is no mode switch here: ExitBootServices leaves the CPU in 64-bit
 * long mode with the firmware's identity map still installed, which is exactly
 * the state _start_uefi in kernel/arch/x86_64/boot/entry.S wants.  An earlier
 * version of this loader dropped to 32-bit protected mode first, in order to
 * satisfy the multiboot contract -- and that detour is where all of the
 * difficulty came from, since every step (building a GDT, reloading CS, clearing
 * EFER.LME/CR4.PAE/CR0.PG in the right order, keeping far pointers free of the
 * absolute relocations a -shared link forbids) can fail as a bare #GP with no
 * console left to report it.  Synthesising a multiboot info block and entering
 * the kernel's own 64-bit entry keeps all of that in the kernel, where the same
 * page-table code already works for the BIOS path.
 *
 * Built with -mabi=ms: UEFI on x86_64 uses the Microsoft calling convention, so
 * efi_main's arguments arrive in RCX and RDX.  AArch64 has no such split, which
 * is why kernel/boot/uefi/aarch64_loader.c needs no such flag.
 */
#include <stdint.h>
#include <stddef.h>

typedef uint64_t efi_status_t;
typedef void *efi_handle_t;
typedef uint16_t efi_char16_t;
typedef uint64_t efi_uintn_t;
typedef uint64_t efi_physical_address_t;

#define EFI_ERROR(s) ((s) >> 63)

/* Values from the UEFI spec.  ANY_PAGES is 0 and MAX_ADDRESS is 1; defining
 * ANY_PAGES as 1 makes the firmware read the zeroed address as a maximum and
 * refuse the allocation. */
#define EFI_ALLOCATE_ANY_PAGES 0
#define EFI_ALLOCATE_MAX_ADDRESS 1
#define EFI_ALLOCATE_ADDRESS 2
#define EFI_LOADER_DATA 2

/* kernel/arch/x86_64/boot/ldscript.ld links PHYS_BASE. */
#ifndef KERNEL_LOAD_ADDRESS
#define KERNEL_LOAD_ADDRESS 0x00200000ULL
#endif

/* _start_uefi sits on the next page boundary after the multiboot header and the
 * 32-bit entry; the linker script aligns .uefi64 to 4096 and exports
 * __uefi_entry_phys = 0x201000. */
#define KERNEL_UEFI_ENTRY (KERNEL_LOAD_ADDRESS + 0x1000ULL)

#define MULTIBOOT_MAGIC 0x2BADB002u
#define MB_INFO_MEM_MAP 0x1u
#define MB_INFO_CMDLINE 0x4u
#define MB_TYPE_AVAILABLE 1u

#define EFI_MAP_BYTES 0x4000u
#define MB_INFO_OFFSET EFI_MAP_BYTES
#define MB_CMDLINE_OFFSET (MB_INFO_OFFSET + 0x800u)
#define SCRATCH_PAGES 16u
#define CMDLINE_MAX 512u
#define MB_INFO_MAX_ENTRIES 64u

struct efi_simple_text_output;
typedef efi_status_t (*efi_output_string_t)(struct efi_simple_text_output *, efi_char16_t *);
struct efi_simple_text_output { void *reset; efi_output_string_t output_string; };

struct efi_guid { uint32_t data1; uint16_t data2; uint16_t data3; uint8_t data4[8]; };

struct efi_memory_descriptor {
    uint32_t type; uint32_t padding;
    efi_physical_address_t physical_start; efi_physical_address_t virtual_start;
    efi_physical_address_t number_of_pages; efi_physical_address_t attribute;
} __attribute__((packed));

struct efi_boot_services {
    uint8_t header[24];
    void *raise_tpl; void *restore_tpl;
    efi_status_t (*allocate_pages)(uint32_t, uint32_t, efi_uintn_t, efi_physical_address_t *);
    efi_status_t (*free_pages)(efi_physical_address_t, efi_uintn_t);
    efi_status_t (*get_memory_map)(efi_uintn_t *, void *, efi_uintn_t *, efi_uintn_t *, uint32_t *);
    void *allocate_pool; void *free_pool; void *create_event; void *set_timer;
    void *wait_for_event; void *signal_event; void *close_event; void *check_event;
    void *install_protocol_interface; void *reinstall_protocol_interface;
    void *uninstall_protocol_interface;
    efi_status_t (*handle_protocol)(efi_handle_t, const struct efi_guid *, void **);
    void *reserved; void *register_protocol_notify; void *locate_handle;
    void *locate_device_path; void *install_configuration_table; void *load_image;
    void *start_image; void *exit; void *unload_image;
    efi_status_t (*exit_boot_services)(efi_handle_t, efi_uintn_t);
};

struct efi_configuration_table { struct efi_guid vendor_guid; void *vendor_table; };

struct efi_system_table {
    uint8_t header[24];
    efi_char16_t *firmware_vendor; uint32_t firmware_revision; uint32_t pad;
    efi_handle_t console_in_handle; void *con_in;
    efi_handle_t console_out_handle; struct efi_simple_text_output *con_out;
    efi_handle_t stderr_handle; struct efi_simple_text_output *std_err;
    void *runtime_services; struct efi_boot_services *boot_services;
    efi_uintn_t number_of_table_entries; struct efi_configuration_table *configuration_table;
};

struct efi_loaded_image {
    uint32_t revision; void *handle; void *image_base; efi_physical_address_t image_size;
    void *image_code; void *image_data; void *unload;
    uint32_t load_options_size; void *load_options;
};

/* EFI_LOADED_IMAGE_PROTOCOL */
static const struct efi_guid loaded_image_guid = {
    0x5b1b31a1U, 0x9562U, 0x11d2U, { 0x8eU,0x3fU,0x00U,0xa0U,0xc9U,0x69U,0x72U,0x3bU } };
/* ACPI publishes the RSDP under either the 2.0 or the 1.0 table GUID. */
static const struct efi_guid acpi20_table_guid = {
    0x8868e871U, 0xe4f1U, 0x11d3U, { 0xbcU,0x22U,0x00U,0x80U,0xc7U,0x3cU,0x88U,0x81U } };
static const struct efi_guid acpi10_table_guid = {
    0xeb9d2d30U, 0x2d88U, 0x11d3U, { 0x9aU,0x16U,0x00U,0x90U,0x27U,0x3fU,0xc1U,0x4dU } };

/* Must match kernel/arch/x86_64/platform/firmware.c. */
struct mb_info {
    uint32_t flags, mem_lower, mem_upper, boot_device, cmdline;
    uint32_t mods_count, mods_addr, syms[4], mmap_length, mmap_addr;
};
struct mb_mmap_entry { uint32_t size; uint64_t addr; uint64_t len; uint32_t type; } __attribute__((packed));

extern const uint8_t _binary_kernel_bin_start[] __attribute__((visibility("hidden")));
extern const uint8_t _binary_kernel_bin_end[] __attribute__((visibility("hidden")));

static void copy_bytes(void *d_, const void *s_, size_t n)
{ uint8_t *d = d_; const uint8_t *s = s_; while (n--) *d++ = *s++; }
static void zero_bytes(void *d_, size_t n)
{ uint8_t *d = d_; while (n--) *d++ = 0; }
static int guid_equal(const struct efi_guid *a, const struct efi_guid *b)
{
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    for (size_t i = 0; i < sizeof(*a); i++) if (x[i] != y[i]) return 0;
    return 1;
}
static void print(struct efi_system_table *st, efi_char16_t *m)
{ if (st && st->con_out && st->con_out->output_string) st->con_out->output_string(st->con_out, m); }

static uintptr_t find_acpi_rsdp(struct efi_system_table *st)
{
    if (!st || !st->configuration_table) return 0;
    for (efi_uintn_t i = 0; i < st->number_of_table_entries; i++) {
        struct efi_configuration_table *t = &st->configuration_table[i];
        if (guid_equal(&t->vendor_guid, &acpi20_table_guid) ||
            guid_equal(&t->vendor_guid, &acpi10_table_guid))
            return (uintptr_t)t->vendor_table;
    }
    return 0;
}

/* Copy LoadOptions out as ASCII, the way GRUB hands a multiboot cmdline over. */
static uint32_t collect_load_options(struct efi_system_table *st, efi_handle_t image, uint64_t scratch)
{
    struct efi_loaded_image *li = NULL;
    if (EFI_ERROR(st->boot_services->handle_protocol(image, &loaded_image_guid, (void **)&li))) return 0;
    if (!li || !li->load_options || !li->load_options_size) return 0;
    size_t chars = li->load_options_size / sizeof(efi_char16_t);
    if (chars >= CMDLINE_MAX) chars = CMDLINE_MAX - 1;
    uint8_t *out = (uint8_t *)(uintptr_t)(scratch + MB_CMDLINE_OFFSET);
    const efi_char16_t *in = (const efi_char16_t *)li->load_options;
    for (size_t i = 0; i < chars; i++) out[i] = (in[i] < 0x80) ? (uint8_t)in[i] : '?';
    out[chars] = '\0';
    return (uint32_t)(scratch + MB_CMDLINE_OFFSET);
}

static int overlaps(uint64_t s, uint64_t l, uint64_t lo, uint64_t hi)
{ return s < hi && lo < s + l; }

efi_status_t efi_main(efi_handle_t image, struct efi_system_table *st)
{
    static efi_char16_t loading[] =
        { 'A','2','0','O','S',':',' ','U','E','F','I',':',' ','l','o','a','d','i','n','g',' ','k','e','r','n','e','l','\r','\n',0 };
    static efi_char16_t failed[] =
        { 'A','2','0','O','S',':',' ','U','E','F','I',':',' ','l','o','a','d',' ','f','a','i','l','e','d','\r','\n',0 };

    struct efi_boot_services *bs = st->boot_services;
    size_t ksize = (size_t)(_binary_kernel_bin_end - _binary_kernel_bin_start);
    efi_uintn_t kernel_pages = (ksize + 4095) / 4096;
    efi_physical_address_t kernel_addr = KERNEL_LOAD_ADDRESS;
    efi_physical_address_t scratch = 0;
    efi_uintn_t map_size, map_key, desc_size;
    uint32_t desc_version;
    efi_status_t status;
    uintptr_t acpi_rsdp = find_acpi_rsdp(st);
    uint32_t cmdline = 0;
    uint64_t kernel_lo = KERNEL_LOAD_ADDRESS, kernel_hi = KERNEL_LOAD_ADDRESS + ksize;

    print(st, loading);

    /* The kernel links for PHYS_BASE, so that address is required rather than
     * preferred: relocating was tried on the aarch64 side and reverted, because
     * the kernel is not position independent. */
    status = bs->allocate_pages(EFI_ALLOCATE_ADDRESS, EFI_LOADER_DATA, kernel_pages, &kernel_addr);
    if (EFI_ERROR(status)) { print(st, failed); return status; }
    copy_bytes((void *)(uintptr_t)kernel_addr, _binary_kernel_bin_start, ksize);

    status = bs->allocate_pages(EFI_ALLOCATE_ANY_PAGES, EFI_LOADER_DATA, SCRATCH_PAGES, &scratch);
    if (EFI_ERROR(status)) { print(st, failed); return status; }

    /* Read the command line BEFORE the memory map.  A firmware call between
     * GetMemoryMap and ExitBootServices invalidates the map key, which makes
     * ExitBootServices fail with EFI_INVALID_PARAMETER. */
    cmdline = collect_load_options(st, image, scratch);

    for (int attempt = 0; attempt < 2; attempt++) {
        map_size = EFI_MAP_BYTES;
        status = bs->get_memory_map(&map_size, (void *)(uintptr_t)scratch,
                                    &map_key, &desc_size, &desc_version);
        if (EFI_ERROR(status)) break;

        uint8_t *map = (uint8_t *)(uintptr_t)scratch;
        uint64_t info_base = scratch + MB_INFO_OFFSET;
        struct mb_mmap_entry *out =
            (struct mb_mmap_entry *)(uintptr_t)(info_base + sizeof(struct mb_info));
        size_t entries = 0, out_used = 0;
        efi_physical_address_t low_kb = 0, high_kb = 0;

        for (size_t off = 0; off + desc_size <= map_size; off += desc_size) {
            const struct efi_memory_descriptor *d =
                (const struct efi_memory_descriptor *)(map + off);
            if (d->type != 1 && d->type != 3 && d->type != 5 && d->type != 7) continue;
            uint64_t len = d->number_of_pages * 4096ULL;
            if (!len) continue;
            /* Memory this loader is using must not be advertised as free. */
            if (overlaps(d->physical_start, len, kernel_lo, kernel_hi) ||
                overlaps(d->physical_start, len, scratch, scratch + SCRATCH_PAGES * 4096ULL)) continue;
            if (entries >= MB_INFO_MAX_ENTRIES) break;
            out[entries].size = (uint32_t)(sizeof(struct mb_mmap_entry) + 4);
            out[entries].addr = d->physical_start;
            out[entries].len = len;
            out[entries].type = MB_TYPE_AVAILABLE;
            entries++; out_used += sizeof(struct mb_mmap_entry);
            if (d->physical_start < 0x100000ULL) low_kb = (d->physical_start + len) / 1024ULL;
            else high_kb = (d->physical_start + len - 0x100000ULL) / 1024ULL;
        }
        zero_bytes(out + entries, 4);   /* marks the end of the map */
        out_used += 4;

        struct mb_info *mi = (struct mb_info *)(uintptr_t)info_base;
        zero_bytes(mi, sizeof(*mi));
        mi->flags = MB_INFO_MEM_MAP | MB_INFO_CMDLINE;
        mi->mem_lower = (uint32_t)low_kb;
        mi->mem_upper = (uint32_t)high_kb;
        mi->mmap_addr = (uint32_t)(info_base + sizeof(struct mb_info));
        mi->mmap_length = (uint32_t)out_used;
        mi->cmdline = cmdline;

        status = bs->exit_boot_services(image, map_key);
        if (!EFI_ERROR(status)) {
            /* Long mode with the identity map still live: hand the kernel its
             * multiboot block and the RSDP, and jump.  No GDT, no mode switch. */
            uint64_t info_pa = info_base;
            uint64_t rsdp_pa = (uint64_t)acpi_rsdp;
            uint64_t entry = KERNEL_UEFI_ENTRY;
            __asm__ __volatile__(
                "movq %0, %%rdi\n\t"
                "movq %1, %%rsi\n\t"
                "movq %2, %%rdx\n\t"
                "jmp  *%2\n\t"
                :
                : "r"(info_pa), "r"(rsdp_pa), "r"(entry)
                : "rdi", "rsi", "rdx", "memory");
            __builtin_unreachable();
        }
    }
    print(st, failed);
    return status;
}
