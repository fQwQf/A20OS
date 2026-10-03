/* Minimal, self-contained AArch64 UEFI loader for the VirtualBox board. */
#include <stdint.h>
#include <stddef.h>

typedef uint64_t efi_status_t;
typedef void *efi_handle_t;
typedef uint16_t efi_char16_t;
typedef uint64_t efi_uintn_t;
typedef uint64_t efi_physical_address_t;

#define EFI_SUCCESS 0
#define EFI_ERROR(s) ((s) >> 63)
#define EFI_ALLOCATE_ADDRESS 2
#define EFI_LOADER_DATA 2

#ifndef KERNEL_LOAD_ADDRESS
#define KERNEL_LOAD_ADDRESS 0x08080000ULL
#endif

struct efi_simple_text_output;
typedef efi_status_t (*efi_output_string_t)(struct efi_simple_text_output *,
                                            efi_char16_t *);
struct efi_simple_text_output {
    void *reset;
    efi_output_string_t output_string;
};

struct efi_guid {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t data4[8];
};

struct efi_boot_services {
    uint8_t header[24];
    void *raise_tpl;
    void *restore_tpl;
    efi_status_t (*allocate_pages)(uint32_t, uint32_t, efi_uintn_t,
                                   efi_physical_address_t *);
    efi_status_t (*free_pages)(efi_physical_address_t, efi_uintn_t);
    efi_status_t (*get_memory_map)(efi_uintn_t *, void *, efi_uintn_t *,
                                   efi_uintn_t *, uint32_t *);
    void *allocate_pool;
    void *free_pool;
    void *create_event;
    void *set_timer;
    void *wait_for_event;
    void *signal_event;
    void *close_event;
    void *check_event;
    void *install_protocol_interface;
    void *reinstall_protocol_interface;
    void *uninstall_protocol_interface;
    efi_status_t (*handle_protocol)(efi_handle_t, const struct efi_guid *,
                                    void **);
    void *reserved;
    void *register_protocol_notify;
    void *locate_handle;
    void *locate_device_path;
    void *install_configuration_table;
    void *load_image;
    void *start_image;
    void *exit;
    void *unload_image;
    efi_status_t (*exit_boot_services)(efi_handle_t, efi_uintn_t);
};

struct efi_system_table {
    uint8_t header[24];
    efi_char16_t *firmware_vendor;
    uint32_t firmware_revision;
    uint32_t pad;
    efi_handle_t console_in_handle;
    void *con_in;
    efi_handle_t console_out_handle;
    struct efi_simple_text_output *con_out;
    efi_handle_t stderr_handle;
    struct efi_simple_text_output *std_err;
    void *runtime_services;
    struct efi_boot_services *boot_services;
    efi_uintn_t number_of_table_entries;
    struct efi_configuration_table *configuration_table;
};


struct efi_configuration_table {
    struct efi_guid vendor_guid;
    void *vendor_table;
};

/* UEFI can publish the RSDP under either the ACPI 2.0 or ACPI 1.0 table
 * GUID.  VirtualBox normally uses ACPI 2.0, but accepting both avoids
 * silently losing PCI discovery on firmware revisions that publish only the
 * legacy GUID. */
static const struct efi_guid acpi20_table_guid = {
    0x8868e871U, 0xe4f1U, 0x11d3U,
    { 0xbcU, 0x22U, 0x00U, 0x80U, 0xc7U, 0x3cU, 0x88U, 0x81U }
};
static const struct efi_guid acpi10_table_guid = {
    0xeb9d2d30U, 0x2d88U, 0x11d3U,
    { 0x9aU, 0x16U, 0x00U, 0x90U, 0x27U, 0x3fU, 0xc1U, 0x4dU }
};

extern const uint8_t _binary_kernel_bin_start[] __attribute__((visibility("hidden")));
extern const uint8_t _binary_kernel_bin_end[] __attribute__((visibility("hidden")));

static void copy_bytes(void *dst, const void *src, size_t count)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (count--)
        *d++ = *s++;
}

static void sync_loaded_code(void *addr, size_t size)
{
    uint64_t ctr;
    __asm__ __volatile__("mrs %0, ctr_el0" : "=r"(ctr));
    size_t dline = (size_t)4U << ((ctr >> 16) & 0xfU);
    size_t iline = (size_t)4U << (ctr & 0xfU);
    uintptr_t start = (uintptr_t)addr & ~((uintptr_t)dline - 1);
    uintptr_t end = ((uintptr_t)addr + size + dline - 1) & ~((uintptr_t)dline - 1);

    for (uintptr_t p = start; p < end; p += dline)
        __asm__ __volatile__("dc cvau, %0" :: "r"(p) : "memory");
    __asm__ __volatile__("dsb ish" ::: "memory");

    start = (uintptr_t)addr & ~((uintptr_t)iline - 1);
    end = ((uintptr_t)addr + size + iline - 1) & ~((uintptr_t)iline - 1);
    for (uintptr_t p = start; p < end; p += iline)
        __asm__ __volatile__("ic ivau, %0" :: "r"(p) : "memory");
    __asm__ __volatile__("dsb ish\n\tisb" ::: "memory");
}

static void print(struct efi_system_table *st, efi_char16_t *message)
{
    if (st && st->con_out && st->con_out->output_string)
        st->con_out->output_string(st->con_out, message);
}

static int guid_equal(const struct efi_guid *a, const struct efi_guid *b)
{
    const uint8_t *ap = (const uint8_t *)a;
    const uint8_t *bp = (const uint8_t *)b;
    for (size_t i = 0; i < sizeof(*a); i++) {
        if (ap[i] != bp[i])
            return 0;
    }
    return 1;
}

/* EFI_LOADED_IMAGE_PROTOCOL, whose LoadOptions is the standard way firmware
 * passes a kernel command line to an EFI application.  Every other A20OS board
 * can be told what to do at boot; this one could not, because the loader threw
 * LoadOptions away and virtualbox-aarch64 answered with a compiled-in string.
 * That is what stopped a root= from ever reaching this platform, and a root= is
 * what a second Alpine disk needs. */
struct efi_loaded_image {
    uint32_t revision;
    void *parent_handle;
    void *system_table;
    void *device_handle;
    void *file_path;
    void *reserved;
    uint32_t load_options_size;
    void *load_options;
};

static const struct efi_guid loaded_image_protocol_guid = {
    0x5b1b31a1U, 0x9562U, 0x11d2U,
    { 0x8eU, 0x3fU, 0x00U, 0xa0U, 0xc9U, 0x69U, 0x72U, 0x3bU }
};

/* Copy the firmware's UTF-16 LoadOptions into a NUL-terminated ASCII buffer the
 * kernel can read directly.  Returns 0 when there is nothing to copy, which is
 * normal: firmware that loads an image without options sets LoadOptionsSize to
 * 0 rather than failing. */
#define A20_EFI_CMDLINE_MAX 256

static efi_physical_address_t collect_load_options(
    struct efi_system_table *st, efi_handle_t image)
{
    struct efi_boot_services *bs;
    struct efi_loaded_image *li = 0;

    if (!st || !image)
        return 0;
    bs = st->boot_services;
    if (!bs ||
        EFI_ERROR(bs->handle_protocol(image, &loaded_image_protocol_guid,
                                     (void **)&li)))
        return 0;
    if (!li || !li->load_options || li->load_options_size == 0)
        return 0;

    /* The size is in bytes of UTF-16, and the string is not NUL-terminated. */
    size_t chars = li->load_options_size / sizeof(efi_char16_t);
    if (chars >= A20_EFI_CMDLINE_MAX)
        chars = A20_EFI_CMDLINE_MAX - 1;

    efi_physical_address_t addr = 0;
    if (EFI_ERROR(bs->allocate_pages(EFI_ALLOCATE_ADDRESS, EFI_LOADER_DATA,
                                     1, &addr)))
        return 0;

    const efi_char16_t *in = (const efi_char16_t *)li->load_options;
    uint8_t *out = (uint8_t *)(uintptr_t)addr;
    for (size_t i = 0; i < chars; i++) {
        efi_char16_t c = in[i];
        out[i] = (c < 0x80) ? (uint8_t)c : '?';
    }
    out[chars] = '\0';
    return addr;
}

static uintptr_t find_acpi_rsdp(struct efi_system_table *st)
{
    if (!st || !st->configuration_table)
        return 0;

    for (efi_uintn_t i = 0; i < st->number_of_table_entries; i++) {
        struct efi_configuration_table *table = &st->configuration_table[i];
        if (guid_equal(&table->vendor_guid, &acpi20_table_guid) ||
            guid_equal(&table->vendor_guid, &acpi10_table_guid))
            return (uintptr_t)table->vendor_table;
    }
    return 0;
}

efi_status_t efi_main(efi_handle_t image, struct efi_system_table *st)
{
    static efi_char16_t loading[] =
        { 'A','2','0','O','S',':',' ','l','o','a','d','i','n','g',' ','k','e','r','n','e','l','\r','\n',0 };
    static efi_char16_t failed[] =
        { 'A','2','0','O','S',':',' ','U','E','F','I',' ','l','o','a','d',' ','f','a','i','l','e','d','\r','\n',0 };
    struct efi_boot_services *bs = st->boot_services;
    efi_physical_address_t address = KERNEL_LOAD_ADDRESS;
    size_t size = (size_t)(_binary_kernel_bin_end - _binary_kernel_bin_start);
    efi_uintn_t pages = (size + 4095) / 4096;
    uint8_t memory_map[32768] __attribute__((aligned(16)));
    efi_uintn_t map_size, map_key, desc_size;
    uint32_t desc_version;
    efi_status_t status;
    uintptr_t acpi_rsdp = find_acpi_rsdp(st);
    efi_physical_address_t cmdline = collect_load_options(st, image);

    print(st, loading);
    /*
     * KERNEL_LOAD_ADDRESS must be available, so a firmware that will not give it
     * up is a hard failure with a message rather than a boot somewhere else.
     *
     * Relocating to a firmware-chosen address was tried here and reverted.  It
     * removed this error -- the allocation succeeded -- and the image then hung
     * in a reboot loop instead, because the kernel is not yet position
     * independent: something in its early path still assumes the image landed at
     * the address it was linked for.  Turning a diagnosable "UEFI load failed"
     * into an endless reboot is strictly worse than failing, so the address stays
     * mandatory until the kernel can be entered anywhere.
     *
     * The audit that found this is still worth having: under QEMU virt, RAM starts
     * at 0x40000000 and nothing lives at 0x08080000, which is why this path is
     * taken at all off VirtualBox.
     */
    status = bs->allocate_pages(EFI_ALLOCATE_ADDRESS, EFI_LOADER_DATA,
                                pages, &address);
    if (EFI_ERROR(status)) {
        print(st, failed);
        return status;
    }
    copy_bytes((void *)(uintptr_t)address, _binary_kernel_bin_start, size);
    sync_loaded_code((void *)(uintptr_t)address, size);

    /* GetMemoryMap changes as allocations occur, so obtain it last and retry
     * ExitBootServices once if firmware invalidates the key asynchronously. */
    for (int attempt = 0; attempt < 2; attempt++) {
        map_size = sizeof(memory_map);
        status = bs->get_memory_map(&map_size, memory_map, &map_key,
                                    &desc_size, &desc_version);
        if (EFI_ERROR(status))
            break;
        status = bs->exit_boot_services(image, map_key);
        if (!EFI_ERROR(status)) {
            /* A20OS owns its EL1 page tables. UEFI uses an identity map, so
             * the final branch remains valid after translation is disabled. */
            __asm__ __volatile__(
                "dsb sy\n\t"
                "mrs x1, sctlr_el1\n\t"
                "bic x1, x1, #1\n\t"
                "bic x1, x1, #(1 << 2)\n\t"
                "bic x1, x1, #(1 << 12)\n\t"
                "msr sctlr_el1, x1\n\t"
                "isb\n\t"
                /* Preserve the UEFI ACPI pointer for the kernel. */
                "mov x0, %0\n\t"
                /* x2 carries the LoadOptions cmdline, or 0 when the firmware
                 * supplied none.  entry.S parks it in aarch64_boot_cmdline. */
                "mov x2, %2\n\t"
                "br %1"
                :: "r"(acpi_rsdp), "r"((uintptr_t)address), "r"(cmdline)
                : "x0", "x1", "x2", "memory");
            __builtin_unreachable();
        }
    }
    print(st, failed);
    return status;
}
