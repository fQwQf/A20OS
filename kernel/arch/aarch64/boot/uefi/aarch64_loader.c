/* Minimal, self-contained AArch64 UEFI loader for the VirtualBox board. */
#include <stdint.h>
#include <stddef.h>
#include "handoff.h"

typedef uint64_t efi_status_t;
typedef void *efi_handle_t;
typedef uint16_t efi_char16_t;
typedef uint64_t efi_uintn_t;
typedef uint64_t efi_physical_address_t;

#define EFI_SUCCESS 0
#define EFI_ERROR(s) ((s) >> 63)

#ifndef A20_LOADER_LOG
#define A20_LOADER_LOG 1
#endif
#define EFI_ALLOCATE_MAX_ADDRESS 1
#define EFI_ALLOCATE_ADDRESS 2
#define EFI_INVALID_PARAMETER (0x8000000000000000ULL | 2)
#define EFI_BAD_BUFFER_SIZE (0x8000000000000000ULL | 4)
#define EFI_LOADER_DATA 2

#ifndef KERNEL_LOAD_ADDRESS
#define KERNEL_LOAD_ADDRESS 0x08080000ULL
#endif
#ifndef KERNEL_MEMORY_BYTES
#error "KERNEL_MEMORY_BYTES must come from the matching ELF PT_LOAD memory span"
#endif

struct efi_simple_text_output;
typedef efi_status_t (*efi_output_string_t)(struct efi_simple_text_output *,
                                            efi_char16_t *);
struct efi_simple_text_output {
    void *reset;
    efi_output_string_t output_string;
};

/* ESP breadcrumb support.  The aarch64 loader uses the same UEFI file
 * protocol as the x86_64 one; only the image builder differs. */
struct efi_file_protocol;
typedef struct efi_file_protocol efi_file_protocol_t;
struct efi_file_protocol {
    uint64_t revision;
    efi_status_t (*open)(efi_file_protocol_t *, efi_file_protocol_t **,
                         efi_char16_t *, uint64_t, uint64_t);
    efi_status_t (*close)(efi_file_protocol_t *);
    efi_status_t (*delete_file)(efi_file_protocol_t *);
    efi_status_t (*read)(efi_file_protocol_t *, efi_uintn_t *, void *);
    efi_status_t (*write)(efi_file_protocol_t *, efi_uintn_t *, void *);
    efi_status_t (*get_position)(efi_file_protocol_t *, uint64_t *);
    efi_status_t (*set_position)(efi_file_protocol_t *, uint64_t);
    void *get_info;
    void *set_info;
    efi_status_t (*flush)(efi_file_protocol_t *);
};
struct efi_simple_file_system {
    uint64_t revision;
    efi_status_t (*open_volume)(struct efi_simple_file_system *,
                                efi_file_protocol_t **);
};
#define EFI_FILE_MODE_READ   0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE  0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE 0x8000000000000000ULL
#define EFI_FILE_DIRECTORY   0x0000000000000010ULL

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
    0x8868e871U,
    0xe4f1U,
    0x11d3U,
    {0xbcU, 0x22U, 0x00U, 0x80U, 0xc7U, 0x3cU, 0x88U, 0x81U}};
static const struct efi_guid acpi10_table_guid = {
    0xeb9d2d30U,
    0x2d88U,
    0x11d3U,
    {0x9aU, 0x16U, 0x00U, 0x90U, 0x27U, 0x3fU, 0xc1U, 0x4dU}};

extern const uint8_t _binary_kernel_bin_start[]
    __attribute__((visibility("hidden")));
extern const uint8_t _binary_kernel_bin_end[]
    __attribute__((visibility("hidden")));

static void copy_bytes(void *dst, const void *src, size_t count) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (count--)
        *d++ = *s++;
}

#include "loader_arch.h"

static void print(struct efi_system_table *st, efi_char16_t *message) {
    if (st && st->con_out && st->con_out->output_string)
        st->con_out->output_string(st->con_out, message);
}

static int guid_equal(const struct efi_guid *a, const struct efi_guid *b) {
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
    0x5b1b31a1U,
    0x9562U,
    0x11d2U,
    {0x8eU, 0x3fU, 0x00U, 0xa0U, 0xc9U, 0x69U, 0x72U, 0x3bU}};
static const struct efi_guid simple_fs_guid = {
    0x964e5b22U,
    0x6459U,
    0x11d2U,
    {0x8eU, 0x39U, 0x00U, 0xa0U, 0xc9U, 0x69U, 0x72U, 0x3bU}};

_Static_assert(offsetof(struct efi_loaded_image, load_options_size) == 48,
               "UEFI LoadOptionsSize ABI");
_Static_assert(offsetof(struct efi_loaded_image, load_options) == 56,
               "UEFI LoadOptions ABI");

static efi_file_protocol_t *g_esp_root;
static int g_esp_ready;

static size_t ascii_len(const char *s) {
    size_t n = 0;
    while (s && s[n])
        n++;
    return n;
}

static void esp_log_text(const char *text) {
    if (!g_esp_ready || !g_esp_root || !text)
        return;
    efi_file_protocol_t *file = 0;
    efi_status_t status = g_esp_root->open(
        g_esp_root, &file, L"\\A20OS\\boot.log",
        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (EFI_ERROR(status) || !file)
        return;
    (void)file->set_position(file, 0xffffffffffffffffULL);
    efi_uintn_t len = ascii_len(text);
    if (len)
        (void)file->write(file, &len, (void *)text);
    (void)file->flush(file);
    (void)file->close(file);
}

static void esp_log_stage(const char *stage) {
    esp_log_text("STAGE ");
    esp_log_text(stage);
    esp_log_text("\n");
}

static void esp_open(struct efi_system_table *st, efi_handle_t image) {
    if (!A20_LOADER_LOG) {
        (void)st;
        (void)image;
        return;
    }
    if (!st || !st->boot_services || !st->boot_services->handle_protocol)
        return;
    struct efi_loaded_image *li = 0;
    if (EFI_ERROR(st->boot_services->handle_protocol(
            image, &loaded_image_protocol_guid, (void **)&li)) ||
        !li || !li->device_handle)
        return;
    struct efi_simple_file_system *fs = 0;
    efi_status_t status = st->boot_services->handle_protocol(
        li->device_handle, &simple_fs_guid, (void **)&fs);
    if (EFI_ERROR(status) || !fs || !fs->open_volume) {
        status = st->boot_services->handle_protocol(
            image, &simple_fs_guid, (void **)&fs);
    }
    if (EFI_ERROR(status) || !fs || !fs->open_volume)
        return;
    if (EFI_ERROR(fs->open_volume(fs, &g_esp_root)) || !g_esp_root)
        return;
    efi_file_protocol_t *dir = 0;
    status = g_esp_root->open(
        g_esp_root, &dir, L"\\A20OS",
        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
        EFI_FILE_DIRECTORY);
    if (!EFI_ERROR(status) && dir)
        (void)dir->close(dir);
    g_esp_ready = 1;
    esp_log_text("\n=== A20OS UEFI boot (aarch64) ===\n");
}

/* The command line shares the bounded handoff allocation. No late firmware
 * allocation can invalidate the GetMemoryMap/ExitBootServices transaction. */
static efi_status_t collect_load_options(struct efi_system_table *st,
                                         efi_handle_t image,
                                         a64_uefi_handoff_t *handoff,
                                         uint64_t physical) {
    struct efi_loaded_image *li = 0;
    efi_status_t status = st->boot_services->handle_protocol(
        image, &loaded_image_protocol_guid, (void **)&li);
    if (EFI_ERROR(status))
        return status;
    if (!li || !li->load_options_size)
        return EFI_SUCCESS;
    if (!li->load_options || (li->load_options_size & 1))
        return EFI_INVALID_PARAMETER;
    size_t available = li->load_options_size / sizeof(efi_char16_t);
    const efi_char16_t *in = li->load_options;
    size_t chars = 0;
    while (chars < available && in[chars]) {
        if (chars >= A64_UEFI_CMDLINE_CAPACITY - 1)
            return EFI_BAD_BUFFER_SIZE;
        chars++;
    }
    if (!chars)
        return EFI_SUCCESS;
    uint8_t *out = (uint8_t *)handoff + A64_UEFI_CMDLINE_OFFSET;
    for (size_t i = 0; i < chars; i++)
        out[i] = in[i] < 0x80 ? (uint8_t)in[i] : '?';
    out[chars] = 0;
    handoff->cmdline_physical = physical + A64_UEFI_CMDLINE_OFFSET;
    handoff->cmdline_bytes = chars + 1;
    return EFI_SUCCESS;
}

static uintptr_t find_acpi_rsdp(struct efi_system_table *st) {
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

efi_status_t efi_main(efi_handle_t image, struct efi_system_table *st) {
    static efi_char16_t loading[] = {'A', '2', '0',  'O',  'S', ':', ' ',
                                     'U', 'E', 'F',  'I',  ' ', 'l', 'o',
                                     'a', 'd', '\r', '\n', 0};
    static efi_char16_t failed[] = {
        'A', '2', '0', 'O', 'S', ':', ' ', 'U', 'E', 'F', 'I',  ' ',  'l',
        'o', 'a', 'd', ' ', 'f', 'a', 'i', 'l', 'e', 'd', '\r', '\n', 0};
    if (!st || !st->boot_services)
        return EFI_INVALID_PARAMETER;
    struct efi_boot_services *bs = st->boot_services;
    uint64_t memory_bytes = KERNEL_MEMORY_BYTES;
    size_t raw_bytes =
        (size_t)(_binary_kernel_bin_end - _binary_kernel_bin_start);
    if (!memory_bytes || raw_bytes > memory_bytes ||
        KERNEL_LOAD_ADDRESS < A64_UEFI_RAM_BASE ||
        KERNEL_LOAD_ADDRESS >= A64_UEFI_RAM_END ||
        memory_bytes > A64_UEFI_RAM_END - KERNEL_LOAD_ADDRESS ||
        memory_bytes > (~(uint64_t)0) - 4095)
        return EFI_BAD_BUFFER_SIZE;
    efi_uintn_t pages = (memory_bytes + 4095) / 4096;
    efi_physical_address_t address = KERNEL_LOAD_ADDRESS;
    esp_open(st, image);
    esp_log_stage("loader");
    print(st, loading);
    esp_log_stage("allocate-kernel");
    efi_status_t status = bs->allocate_pages(EFI_ALLOCATE_ADDRESS,
                                             EFI_LOADER_DATA, pages, &address);
    if (EFI_ERROR(status)) {
        print(st, failed);
        return status;
    }
    if (address != KERNEL_LOAD_ADDRESS)
        return EFI_INVALID_PARAMETER;
    esp_log_stage("copy-kernel");
    uint8_t *kernel = (uint8_t *)(uintptr_t)address;
    for (size_t i = 0; i < pages * 4096; i++)
        kernel[i] = 0;
    copy_bytes(kernel, _binary_kernel_bin_start, raw_bytes);

    /* Both the map and options must be in the kernel's ordinary RAM alias. */
    esp_log_stage("allocate-handoff");
    efi_physical_address_t physical = A64_UEFI_RAM_END - 1;
    status = bs->allocate_pages(EFI_ALLOCATE_MAX_ADDRESS, EFI_LOADER_DATA,
                                A64_UEFI_HANDOFF_BYTES / 4096, &physical);
    if (EFI_ERROR(status)) {
        bs->free_pages(address, pages);
        print(st, failed);
        return status;
    }
    if (physical < A64_UEFI_RAM_BASE ||
        physical > A64_UEFI_RAM_END - A64_UEFI_HANDOFF_BYTES ||
        (physical & 4095))
        return EFI_INVALID_PARAMETER;
    a64_uefi_handoff_t *handoff = (void *)(uintptr_t)physical;
    for (size_t i = 0; i < A64_UEFI_HANDOFF_BYTES; i++)
        ((uint8_t *)handoff)[i] = 0;
    handoff->magic = A64_UEFI_HANDOFF_MAGIC;
    handoff->version = A64_UEFI_HANDOFF_VERSION;
    handoff->header_bytes = sizeof(*handoff);
    handoff->total_bytes = A64_UEFI_HANDOFF_BYTES;
    handoff->map_offset = A64_UEFI_MAP_OFFSET;
    handoff->kernel_physical = address;
    handoff->kernel_bytes = memory_bytes;
    esp_log_stage("collect-cmdline");
    status = collect_load_options(st, image, handoff, physical);
    if (EFI_ERROR(status)) {
        bs->free_pages(physical, A64_UEFI_HANDOFF_BYTES / 4096);
        bs->free_pages(address, pages);
        print(st, failed);
        return status;
    }
    uintptr_t rsdp = find_acpi_rsdp(st);
    sync_loaded_code(kernel, (size_t)(pages * 4096));

    /* No allocation, logging, or other firmware service is permitted between
     * the successful snapshot and ExitBootServices, including on a stale-key
     * retry. An oversized map fails instead of accepting a partial snapshot. */
    for (int attempt = 0; attempt < 3; attempt++) {
        esp_log_stage("memory-map");
        efi_uintn_t bytes = A64_UEFI_MAP_CAPACITY, key = 0, stride = 0;
        uint32_t version = 0;
        status =
            bs->get_memory_map(&bytes, (uint8_t *)handoff + A64_UEFI_MAP_OFFSET,
                               &key, &stride, &version);
        if (EFI_ERROR(status))
            return status;
        if (!bytes || bytes > A64_UEFI_MAP_CAPACITY ||
            stride < sizeof(a64_uefi_descriptor_t) || stride > 256 ||
            (stride & 7) || bytes % stride ||
            bytes / stride > A64_UEFI_MAX_DESCRIPTORS || version != 1)
            return EFI_INVALID_PARAMETER;
        handoff->map_bytes = (uint32_t)bytes;
        handoff->descriptor_bytes = (uint32_t)stride;
        handoff->descriptor_version = version;
        esp_log_stage("exit-boot-services");
        esp_log_text("RESULT handoff=attempt\n");
        status = bs->exit_boot_services(image, key);
        if (!EFI_ERROR(status)) {
            sync_loaded_code(handoff, A64_UEFI_HANDOFF_BYTES);
            enter_kernel((uintptr_t)address, rsdp,
                         (uintptr_t)handoff->cmdline_physical,
                         (uintptr_t)physical);
        }
    }
    return status;
}
