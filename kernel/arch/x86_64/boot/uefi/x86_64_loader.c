/* x86_64 UEFI loader. Built with -mabi=ms for the firmware protocol ABI.
 * Export a standard Multiboot v1 memory map, copy command line options and
 * preserve the ACPI RSDP. The kernel has a fixed physical address selected by
 * the architecture build configuration. No destination write occurs before
 * ExitBootServices; a private identity map and scratch stack keep the copy
 * independent of reclaimed firmware page tables and stack memory.
 */
#include <stdint.h>
#include <stddef.h>
#include "../../include/kernel_load.h"
#include "../../../../include/boot/framebuffer.h"

typedef uint64_t efi_status_t;
typedef void *efi_handle_t;
typedef uint16_t efi_char16_t;
typedef uint64_t efi_uintn_t;
typedef uint64_t efi_physical_address_t;

#define EFI_ERROR(s) ((s) >> 63)

#ifndef A20_LOADER_LOG
#define A20_LOADER_LOG 1
#endif

/* Values from the UEFI spec.  ANY_PAGES is 0 and MAX_ADDRESS is 1; defining
 * ANY_PAGES as 1 makes the firmware read the zeroed address as a maximum and
 * refuse the allocation. */
#define EFI_ALLOCATE_ANY_PAGES 0
#define EFI_ALLOCATE_MAX_ADDRESS 1
#define EFI_ALLOCATE_ADDRESS 2
#define EFI_LOADER_DATA 2

/* kernel/arch/x86_64/boot/ldscript.ld links PHYS_BASE. */
#ifndef KERNEL_LOAD_ADDRESS
#define KERNEL_LOAD_ADDRESS ((uint64_t)X86_KERNEL_LOAD_ADDRESS)
#endif

/* _start_uefi sits on the next page boundary after the multiboot header and the
 * 32-bit entry; the linker script aligns .uefi64 to 4096 and exports
 * __uefi_entry_phys = PHYS_BASE + 0x1000. */
#define KERNEL_UEFI_ENTRY (KERNEL_LOAD_ADDRESS + 0x1000ULL)
#ifndef KERNEL_MEMORY_BYTES
#error "KERNEL_MEMORY_BYTES must describe the complete ELF PT_LOAD memory span"
#endif

#define MULTIBOOT_MAGIC 0x2BADB002u
#define MB_INFO_MEM_MAP 0x40u
#define MB_INFO_CMDLINE 0x4u
#define MB_TYPE_AVAILABLE 1u

#define EFI_MAP_BYTES 0x10000u
#define MB_INFO_OFFSET EFI_MAP_BYTES
#define MB_CMDLINE_OFFSET (MB_INFO_OFFSET + 0x4000u)
#define SCRATCH_PAGES 64u
#define CMDLINE_MAX 512u
#define MB_INFO_MAX_ENTRIES 512u

struct efi_simple_text_output;
typedef efi_status_t (*efi_output_string_t)(struct efi_simple_text_output *,
                                            efi_char16_t *);
struct efi_simple_text_output {
    void *reset;
    efi_output_string_t output_string;
};

/* EFI_FILE_PROTOCOL and EFI_SIMPLE_FILE_SYSTEM_PROTOCOL, enough to append a
 * breadcrumb file and a boot counter to the ESP.  These are boot services, so
 * they are valid up to ExitBootServices and never after it. */
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

struct efi_memory_descriptor {
    uint32_t type;
    uint32_t padding;
    efi_physical_address_t physical_start;
    efi_physical_address_t virtual_start;
    efi_physical_address_t number_of_pages;
    efi_physical_address_t attribute;
} __attribute__((packed));

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
    void *post_exit_slots[10];
    efi_status_t (*locate_protocol)(const struct efi_guid *, void *, void **);
};

_Static_assert(offsetof(struct efi_boot_services, locate_protocol) == 320,
               "UEFI LocateProtocol ABI");

struct efi_configuration_table {
    struct efi_guid vendor_guid;
    void *vendor_table;
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

struct efi_loaded_image {
    uint32_t revision;
    efi_handle_t parent_handle;
    struct efi_system_table *system_table;
    efi_handle_t device_handle;
    void *file_path, *reserved;
    uint32_t load_options_size;
    void *load_options;
    void *image_base;
    uint64_t image_size;
    uint32_t image_code_type, image_data_type;
    void *unload;
};
_Static_assert(offsetof(struct efi_loaded_image, load_options_size) == 48,
               "UEFI loaded image LoadOptionsSize ABI");
_Static_assert(offsetof(struct efi_loaded_image, load_options) == 56,
               "UEFI loaded image LoadOptions ABI");

/* EFI_LOADED_IMAGE_PROTOCOL */
static const struct efi_guid loaded_image_guid = {
    0x5b1b31a1U,
    0x9562U,
    0x11d2U,
    {0x8eU, 0x3fU, 0x00U, 0xa0U, 0xc9U, 0x69U, 0x72U, 0x3bU}};
/* ACPI publishes the RSDP under either the 2.0 or the 1.0 table GUID. */
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
/* EFI_SIMPLE_FILE_SYSTEM_PROTOCOL */
static const struct efi_guid simple_fs_guid = {
    0x964e5b22U,
    0x6459U,
    0x11d2U,
    {0x8eU, 0x39U, 0x00U, 0xa0U, 0xc9U, 0x69U, 0x72U, 0x3bU}};
/* EFI_FILE_SYSTEM_INFO_ID */
static const struct efi_guid file_system_info_guid = {
    0x09576e93U,
    0x6d3fU,
    0x11d2U,
    {0x8eU, 0x39U, 0x00U, 0xa0U, 0xc9U, 0x69U, 0x72U, 0x3bU}};

struct efi_file_system_info {
    uint64_t size;
    uint8_t read_only;
    uint8_t pad[7];
    uint64_t volume_size;
    uint64_t free_space;
    uint32_t block_size;
    efi_char16_t volume_label[1];
};
_Static_assert(offsetof(struct efi_file_system_info, volume_size) == 16,
               "UEFI file-system-info VolumeSize ABI");
_Static_assert(offsetof(struct efi_file_system_info, volume_label) == 36,
               "UEFI file-system-info VolumeLabel ABI");

typedef efi_status_t (*efi_locate_handle_t)(uint32_t, const struct efi_guid *,
                                            void *, efi_uintn_t *,
                                            efi_handle_t *);
typedef efi_status_t (*efi_file_get_info_t)(efi_file_protocol_t *,
                                            const struct efi_guid *,
                                            efi_uintn_t *, void *);

/* Must match kernel/arch/x86_64/platform/firmware.c. */
struct mb_info {
    uint32_t flags, mem_lower, mem_upper, boot_device, cmdline;
    uint32_t mods_count, mods_addr, syms[4], mmap_length, mmap_addr;
};
struct mb_mmap_entry {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type;
} __attribute__((packed));
_Static_assert(sizeof(struct mb_mmap_entry) == 24, "Multiboot mmap record ABI");
_Static_assert(sizeof(struct mb_info) +
                       MB_INFO_MAX_ENTRIES * sizeof(struct mb_mmap_entry) <=
                   MB_CMDLINE_OFFSET - MB_INFO_OFFSET,
               "handoff map overlaps command line");
_Static_assert(MB_CMDLINE_OFFSET + CMDLINE_MAX <= SCRATCH_PAGES * 4096u,
               "handoff exceeds scratch allocation");

extern const uint8_t _binary_kernel_bin_start[]
    __attribute__((visibility("hidden")));
extern const uint8_t _binary_kernel_bin_end[]
    __attribute__((visibility("hidden")));

static void copy_bytes(void *d_, const void *s_, size_t n) {
    uint8_t *d = d_;
    const uint8_t *s = s_;
    while (n--)
        *d++ = *s++;
}
static void zero_bytes(void *d_, size_t n) {
    uint8_t *d = d_;
    while (n--)
        *d++ = 0;
}
static int guid_equal(const struct efi_guid *a, const struct efi_guid *b) {
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    for (size_t i = 0; i < sizeof(*a); i++)
        if (x[i] != y[i])
            return 0;
    return 1;
}
static void print(struct efi_system_table *st, efi_char16_t *m) {
    if (st && st->con_out && st->con_out->output_string)
        st->con_out->output_string(st->con_out, m);
}

/* ESP breadcrumb state.  The file is opened per line: the loader writes a
 * handful of lines and this keeps every stage durable even if the next
 * firmware call never returns. */
static efi_file_protocol_t *g_esp_root;
static int g_esp_ready;
static struct efi_system_table *g_esp_st;
static void report_failure(struct efi_system_table *st, const char *stage,
                           efi_status_t status);

/* Early JSONL on the "A20OS log" FAT32 partition.  The kernel cannot touch
 * block storage until after firmware enumeration, PCI/USB probing and VFS
 * setup; this file starts at the loader stage, so a machine that dies before
 * mount_persistent_log() still leaves structured evidence on the stick.  If
 * the log partition is absent the same records go to the ESP instead. */
static efi_file_protocol_t *g_early_file;
static uint32_t g_early_bootcount;
static unsigned g_early_seq;
static int g_early_ready;

static size_t early_append(char *dst, size_t cap, size_t at,
                           const char *src) {
    if (!dst || !cap)
        return 0;
    while (src && *src && at + 1 < cap)
        dst[at++] = *src++;
    dst[at] = '\0';
    return at;
}

static size_t early_u32(char *dst, size_t cap, size_t at, uint32_t value) {
    char digits[16];
    size_t n = 0;
    if (!value)
        digits[n++] = '0';
    while (value) {
        digits[n++] = (char)('0' + value % 10U);
        value /= 10U;
    }
    while (n && at + 1 < cap)
        dst[at++] = digits[--n];
    if (cap)
        dst[at] = '\0';
    return at;
}

static size_t early_hex64(char *dst, size_t cap, size_t at, uint64_t value) {
    static const char hex[] = "0123456789abcdef";
    at = early_append(dst, cap, at, "0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        if (at + 1 >= cap)
            break;
        dst[at++] = hex[(value >> shift) & 15U];
    }
    if (cap)
        dst[at] = '\0';
    return at;
}

static void early_raw(const char *text, size_t len) {
    if (!g_early_ready || !g_early_file || !text || !len)
        return;
    (void)g_early_file->set_position(g_early_file, 0xffffffffffffffffULL);
    efi_uintn_t count = len;
    efi_status_t status = g_early_file->write(g_early_file, &count,
                                              (void *)text);
    if (EFI_ERROR(status)) {
        (void)g_early_file->close(g_early_file);
        g_early_file = 0;
        g_early_ready = 0;
        return;
    }
    /* Flush each record: the next firmware call may never return. */
    (void)g_early_file->flush(g_early_file);
}

static void early_line(const char *level, const char *msg) {
    char line[384];
    size_t at = 0;
    at = early_append(line, sizeof(line), at,
                      "{\"v\":1,\"boot\":\"loader-");
    at = early_u32(line, sizeof(line), at, g_early_bootcount);
    at = early_append(line, sizeof(line), at, "\",\"bootcount\":");
    at = early_u32(line, sizeof(line), at, g_early_bootcount);
    at = early_append(line, sizeof(line), at, ",\"seq\":");
    at = early_u32(line, sizeof(line), at, g_early_seq++);
    at = early_append(line, sizeof(line), at,
                      ",\"mono_ns\":0,\"rt\":0,\"lvl\":\"");
    at = early_append(line, sizeof(line), at, level ? level : "info");
    at = early_append(line, sizeof(line), at,
                      "\",\"src\":\"UEFI\",\"msg\":\"");
    for (const char *p = msg; p && *p && at + 8 < sizeof(line); p++) {
        if (*p == '"' || *p == '\\') {
            line[at++] = '\\';
            line[at++] = *p;
        } else if (*p == '\n') {
            line[at++] = '\\';
            line[at++] = 'n';
        } else {
            line[at++] = *p;
        }
    }
    at = early_append(line, sizeof(line), at, "\"}\n");
    early_raw(line, at);
}

static void early_stage(const char *stage) {
    char msg[96];
    size_t at = early_append(msg, sizeof(msg), 0, "STAGE ");
    at = early_append(msg, sizeof(msg), at, stage ? stage : "?");
    (void)at;
    early_line("info", msg);
}

static void early_fail(const char *stage, efi_status_t status) {
    char msg[128];
    size_t at = early_append(msg, sizeof(msg), 0, "FAIL stage=");
    at = early_append(msg, sizeof(msg), at, stage ? stage : "?");
    at = early_append(msg, sizeof(msg), at, " status=");
    at = early_hex64(msg, sizeof(msg), at, status);
    (void)at;
    early_line("err", msg);
}

static int volume_label_is_log(const efi_char16_t *label) {
    static const char want[] = "A20OSLOG";
    if (!label)
        return 0;
    for (unsigned i = 0; i < sizeof(want) - 1; i++)
        if (label[i] != (efi_char16_t)(unsigned char)want[i])
            return 0;
    return 1;
}

static void ascii_to_utf16(efi_char16_t *out, size_t cap, const char *src) {
    size_t i = 0;
    if (!out || !cap)
        return;
    while (src && *src && i + 1 < cap)
        out[i++] = (efi_char16_t)(unsigned char)*src++;
    out[i] = 0;
}

static int early_open_path(efi_file_protocol_t *root, const efi_char16_t *path) {
    if (!root || !path)
        return 0;
    efi_status_t status = root->open(
        root, &g_early_file, (efi_char16_t *)path,
        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (EFI_ERROR(status) || !g_early_file) {
        g_early_file = 0;
        return 0;
    }
    g_early_ready = 1;
    return 1;
}

static void early_open(uint32_t bootcount) {
    g_early_bootcount = bootcount;
    g_early_seq = 0;
    g_early_ready = 0;
    g_early_file = 0;

    char name[64];
    size_t at = early_append(name, sizeof(name), 0, "\\early-");
    at = early_u32(name, sizeof(name), at, bootcount);
    at = early_append(name, sizeof(name), at, ".jsonl");

    efi_char16_t path[64];
    ascii_to_utf16(path, sizeof(path) / sizeof(path[0]), name);

    struct efi_boot_services *bs = g_esp_st ? g_esp_st->boot_services : 0;
    if (bs && bs->locate_handle) {
        efi_handle_t handles[32];
        efi_uintn_t size = sizeof(handles);
        efi_status_t status = ((efi_locate_handle_t)bs->locate_handle)(
            2 /* ByProtocol */, &simple_fs_guid, 0, &size, handles);
        if (!EFI_ERROR(status)) {
            unsigned count = (unsigned)(size / sizeof(handles[0]));
            for (unsigned i = 0; i < count; i++) {
                struct efi_simple_file_system *fs = 0;
                if (EFI_ERROR(bs->handle_protocol(handles[i], &simple_fs_guid,
                                                  (void **)&fs)) || !fs)
                    continue;
                efi_file_protocol_t *root = 0;
                if (EFI_ERROR(fs->open_volume(fs, &root)) || !root)
                    continue;
                uint8_t info_buf[512];
                efi_uintn_t info_size = sizeof(info_buf);
                struct efi_file_system_info *info =
                    (struct efi_file_system_info *)info_buf;
                status = ((efi_file_get_info_t)root->get_info)(
                    root, &file_system_info_guid, &info_size, info_buf);
                int match = !EFI_ERROR(status) && info_size >= 52 &&
                            info->size >= 52 &&
                            volume_label_is_log(info->volume_label);
                if (match) {
                    (void)early_open_path(root, path);
                    (void)root->close(root);
                    if (g_early_ready)
                        return;
                } else {
                    (void)root->close(root);
                }
            }
        }
    }

    /* No log partition (or its FAT driver is read-only): keep the same JSONL
     * records on the ESP, next to the human-readable boot.log. */
    if (g_esp_ready && g_esp_root) {
        char esp_name[80];
        at = early_append(esp_name, sizeof(esp_name), 0, "\\A20OS\\early-");
        at = early_u32(esp_name, sizeof(esp_name), at, bootcount);
        at = early_append(esp_name, sizeof(esp_name), at, ".jsonl");
        ascii_to_utf16(path, sizeof(path) / sizeof(path[0]), esp_name);
        (void)early_open_path(g_esp_root, path);
    }
}

static void early_close(void) {
    if (g_early_file) {
        (void)g_early_file->flush(g_early_file);
        (void)g_early_file->close(g_early_file);
        g_early_file = 0;
    }
    g_early_ready = 0;
}

static size_t ascii_len(const char *s) {
    size_t n = 0;
    while (s && s[n])
        n++;
    return n;
}

static void esp_log_text(const char *text) {
    if (!g_esp_ready || !g_esp_root || !text)
        return;
    efi_file_protocol_t *file = NULL;
    efi_status_t status = g_esp_root->open(
        g_esp_root, &file, L"\\A20OS\\boot.log",
        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (EFI_ERROR(status) || !file) {
        static int reported;
        if (!reported && g_esp_st) {
            reported = 1;
            report_failure(g_esp_st, "ESP open boot.log", status);
        }
        return;
    }
    (void)file->set_position(file, 0xffffffffffffffffULL);
    efi_uintn_t len = ascii_len(text);
    if (len)
        (void)file->write(file, &len, (void *)text);
    (void)file->flush(file);
    (void)file->close(file);
}

static void esp_log_hex(efi_status_t value) {
    char buf[32];
    size_t n = 0;
    buf[n++] = '0';
    buf[n++] = 'x';
    for (int shift = 60; shift >= 0; shift -= 4)
        buf[n++] = "0123456789abcdef"[(value >> shift) & 15];
    buf[n] = '\0';
    esp_log_text(buf);
}

static void esp_log_stage(const char *stage) {
    esp_log_text("STAGE ");
    esp_log_text(stage);
    esp_log_text("\n");
    early_stage(stage);
}

static void esp_log_fail(const char *stage, efi_status_t status) {
    esp_log_text("FAIL stage=");
    esp_log_text(stage ? stage : "?");
    esp_log_text(" status=");
    esp_log_hex(status);
    esp_log_text("\n");
    early_fail(stage, status);
}

static uint32_t esp_read_bootcount(void) {
    if (!g_esp_ready || !g_esp_root)
        return 0;
    efi_file_protocol_t *file = NULL;
    efi_status_t status = g_esp_root->open(
        g_esp_root, &file, L"\\A20OS\\bootcount", EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status) || !file)
        return 0;
    char buf[32] = {0};
    efi_uintn_t len = sizeof(buf) - 1;
    (void)file->read(file, &len, buf);
    (void)file->close(file);
    uint32_t value = 0;
    for (efi_uintn_t i = 0; i < len && buf[i] && buf[i] != '\n'; i++) {
        char c = buf[i];
        unsigned digit;
        if (c >= '0' && c <= '9') digit = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') digit = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = (unsigned)(c - 'A' + 10);
        else break;
        value = value * 16U + digit;
    }
    return value;
}

static void esp_write_bootcount(uint32_t value) {
    if (!g_esp_ready || !g_esp_root)
        return;
    efi_file_protocol_t *file = NULL;
    efi_status_t status = g_esp_root->open(
        g_esp_root, &file, L"\\A20OS\\bootcount",
        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (EFI_ERROR(status) || !file)
        return;
    (void)file->set_position(file, 0);
    char buf[16];
    size_t n = 0;
    for (int shift = 28; shift >= 0; shift -= 4)
        buf[n++] = "0123456789abcdef"[(value >> shift) & 15];
    buf[n++] = '\n';
    efi_uintn_t len = n;
    (void)file->write(file, &len, buf);
    (void)file->flush(file);
    (void)file->close(file);
}

static void esp_open(struct efi_system_table *st, efi_handle_t image) {
    if (!A20_LOADER_LOG) {
        (void)st;
        (void)image;
        return;
    }
    g_esp_st = st;
    struct efi_loaded_image *li = NULL;
    if (!st || !st->boot_services || !st->boot_services->handle_protocol)
        return;
    if (EFI_ERROR(st->boot_services->handle_protocol(
            image, &loaded_image_guid, (void **)&li)) ||
        !li || !li->device_handle) {
        report_failure(st, "ESP loaded-image handle", 1);
        return;
    }
    struct efi_simple_file_system *fs = NULL;
    efi_status_t status = st->boot_services->handle_protocol(
        li->device_handle, &simple_fs_guid, (void **)&fs);
    if (EFI_ERROR(status) || !fs || !fs->open_volume) {
        /* Some firmware publishes the filesystem on the image handle rather
         * than the partition handle; try it before giving up. */
        status = st->boot_services->handle_protocol(
            image, &simple_fs_guid, (void **)&fs);
    }
    if (EFI_ERROR(status) || !fs || !fs->open_volume) {
        report_failure(st, "ESP simple-filesystem", status);
        return;
    }
    status = fs->open_volume(fs, &g_esp_root);
    if (EFI_ERROR(status) || !g_esp_root) {
        report_failure(st, "ESP open-volume", status);
        return;
    }
    efi_file_protocol_t *dir = NULL;
    status = g_esp_root->open(
        g_esp_root, &dir, L"\\A20OS",
        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
        EFI_FILE_DIRECTORY);
    if (!EFI_ERROR(status) && dir)
        (void)dir->close(dir);
    else
        report_failure(st, "ESP create A20OS dir", status);
    g_esp_ready = 1;
    esp_log_text("\n=== A20OS UEFI boot ===\n");
    esp_log_text("loader=");
    esp_log_text("x86_64");
    esp_log_text("\n");
}

static void append_cmdline_bootcount(uint32_t cmdline, uint32_t bootcount) {
    if (!cmdline)
        return;
    char *buf = (char *)(uintptr_t)cmdline;
    size_t len = ascii_len(buf);
    char suffix[48];
    size_t n = 0;
    const char *key = " a20.bootcount=";
    while (*key)
        suffix[n++] = *key++;
    char digits[16];
    size_t d = 0;
    if (!bootcount)
        digits[d++] = '0';
    while (bootcount) {
        digits[d++] = (char)('0' + (bootcount % 10));
        bootcount /= 10;
    }
    while (d)
        suffix[n++] = digits[--d];
    suffix[n] = '\0';
    if (len + n + 1 >= CMDLINE_MAX)
        return;
    for (size_t i = 0; i <= n; i++)
        buf[len + i] = suffix[i];
}

/* Keep failed firmware calls diagnosable on a serial-backed EFI console. */
static void report_failure(struct efi_system_table *st, const char *stage,
                           efi_status_t status) {
    efi_char16_t line[128];
    const char *prefix = "A20OS: UEFI: ";
    size_t n = 0;
    while (*prefix)
        line[n++] = (uint8_t)*prefix++;
    while (*stage && n < 96)
        line[n++] = (uint8_t)*stage++;
    line[n++] = ' ';
    line[n++] = '0';
    line[n++] = 'x';
    for (int shift = 60; shift >= 0; shift -= 4)
        line[n++] = "0123456789abcdef"[(status >> shift) & 15];
    line[n++] = '\r';
    line[n++] = '\n';
    line[n] = 0;
    print(st, line);
    esp_log_fail(stage, status);
}

static uintptr_t find_acpi_rsdp(struct efi_system_table *st) {
    if (!st || !st->configuration_table)
        return 0;
    for (efi_uintn_t i = 0; i < st->number_of_table_entries; i++) {
        struct efi_configuration_table *t = &st->configuration_table[i];
        if (guid_equal(&t->vendor_guid, &acpi20_table_guid) ||
            guid_equal(&t->vendor_guid, &acpi10_table_guid))
            return (uintptr_t)t->vendor_table;
    }
    return 0;
}

/* Copy LoadOptions out as ASCII, the way GRUB hands a multiboot cmdline over. */
static uint32_t collect_load_options(struct efi_system_table *st,
                                     efi_handle_t image, uint64_t scratch) {
    uint8_t *out = (uint8_t *)(uintptr_t)(scratch + MB_CMDLINE_OFFSET);
    out[0] = '\0';
    struct efi_loaded_image *li = NULL;
    if (EFI_ERROR(st->boot_services->handle_protocol(image, &loaded_image_guid,
                                                     (void **)&li)))
        return (uint32_t)(scratch + MB_CMDLINE_OFFSET);
    if (!li || !li->load_options || !li->load_options_size)
        return (uint32_t)(scratch + MB_CMDLINE_OFFSET);
    size_t chars = li->load_options_size / sizeof(efi_char16_t);
    if (chars >= CMDLINE_MAX)
        chars = CMDLINE_MAX - 1;
    const efi_char16_t *in = (const efi_char16_t *)li->load_options;
    for (size_t i = 0; i < chars; i++)
        out[i] = (in[i] < 0x80) ? (uint8_t)in[i] : '?';
    out[chars] = '\0';
    return (uint32_t)(scratch + MB_CMDLINE_OFFSET);
}

/* UEFI 2.10 memory types are reclaimable only after ExitBootServices. The
 * kernel's own LoaderData remains physical RAM: PFA reserves the prefix up to
 * kernel_end, where it also places metadata. Scratch is a live handoff buffer
 * and must instead be cut out. No EFI runtime or ACPI/NVS page is reclaimed. */
static int export_entry(struct mb_mmap_entry *out, size_t capacity,
                        size_t *count, uint64_t base, uint64_t end,
                        uint32_t type) {
    if (end <= base)
        return 0;
    if (*count == capacity)
        return -2;
    out[*count] = (struct mb_mmap_entry){20, base, end - base, type};
    (*count)++;
    return 0;
}
static int export_memory_map(const void *map_, size_t bytes, size_t stride,
                             uint32_t version, uint64_t scratch,
                             uint64_t scratch_bytes, struct mb_mmap_entry *out,
                             size_t capacity, size_t *count) {
    *count = 0;
    if (!map_ || !out || version != 1 ||
        stride < sizeof(struct efi_memory_descriptor) || !bytes ||
        bytes % stride || !scratch_bytes ||
        scratch_bytes > UINT64_MAX - scratch)
        return -1;
    const uint8_t *map = map_;
    uint64_t scratch_end = scratch + scratch_bytes;
    for (size_t offset = 0; offset < bytes; offset += stride) {
        struct efi_memory_descriptor descriptor;
        copy_bytes(&descriptor, map + offset, sizeof(descriptor));
        uint64_t base = descriptor.physical_start,
                 pages = descriptor.number_of_pages;
        if ((base & 4095) || pages > UINT64_MAX / 4096)
            goto invalid;
        uint64_t length = pages * 4096;
        if (length > UINT64_MAX - base)
            goto invalid;
        uint64_t end = base + length;
        if (!length)
            continue;
        int usable = (descriptor.type == 1 || descriptor.type == 2 ||
                      descriptor.type == 3 || descriptor.type == 4 ||
                      descriptor.type == 7) &&
                     !(descriptor.attribute & (1ULL << 63));
        int rc;
        if (!usable || end <= scratch || base >= scratch_end) {
            rc = export_entry(out, capacity, count, base, end, usable ? 1 : 2);
            if (rc)
                goto failed;
            continue;
        }
        uint64_t cut_base = base > scratch ? base : scratch,
                 cut_end = end < scratch_end ? end : scratch_end;
        rc = export_entry(out, capacity, count, base, cut_base, 1);
        if (!rc)
            rc = export_entry(out, capacity, count, cut_base, cut_end, 2);
        if (!rc)
            rc = export_entry(out, capacity, count, cut_end, end, 1);
        if (rc)
            goto failed;
    }
    /* Even an inconsistent firmware descriptor list cannot expose the live
     * handoff buffer as free RAM. Reserved wins in the Multiboot consumer. */
    if (export_entry(out, capacity, count, scratch, scratch_end, 2))
        goto failed;
    return 0;
failed:
    *count = 0;
    return -2;
invalid:
    *count = 0;
    return -1;
}

/* The post-EBS copy uses neither the firmware stack nor its page tables: both
 * may occupy the fixed-address kernel's BSS. Scratch owns all live state. */
#define TRANSITION_OFFSET (MB_CMDLINE_OFFSET + CMDLINE_MAX)
#define TRANSITION_PT_OFFSET 0x18000u
#define TRANSITION_PT_PAGES 32u
#define TRANSITION_STACK_OFFSET                                                \
    (TRANSITION_PT_OFFSET + TRANSITION_PT_PAGES * 4096u)
struct transition {
    uint64_t source, file_bytes, memory_bytes, info, rsdp, framebuffer;
};
_Static_assert(TRANSITION_OFFSET + 0x100 + sizeof(boot_framebuffer_t) <=
                   TRANSITION_PT_OFFSET,
               "transition overlaps page tables");
_Static_assert(TRANSITION_STACK_OFFSET + 16384 <= SCRATCH_PAGES * 4096u,
               "transition needs a private stack");

static int overlaps(uint64_t base, uint64_t bytes, uint64_t other,
                    uint64_t other_bytes) {
    return bytes && other_bytes && base < other + other_bytes &&
           other < base + bytes;
}
static int reclaimable(const struct efi_memory_descriptor *d) {
    return (d->type == 1 || d->type == 2 || d->type == 3 || d->type == 4 ||
            d->type == 7) &&
           !(d->attribute & (1ULL << 63));
}
/* Called only after export_memory_map validated descriptor sizes/arithmetic.
 * Every byte must be described, and any conflicting reserved row wins. */
static int target_reclaimable(const void *map, size_t bytes, size_t stride,
                              uint64_t base, uint64_t length) {
    if (!length || length > UINT64_MAX - base)
        return 0;
    uint64_t end = base + length, cursor = base;
    for (size_t offset = 0; offset < bytes; offset += stride) {
        struct efi_memory_descriptor d;
        copy_bytes(&d, (const uint8_t *)map + offset, sizeof(d));
        if (overlaps(base, length, d.physical_start,
                     d.number_of_pages * 4096) &&
            !reclaimable(&d))
            return 0;
    }
    while (cursor < end) {
        uint64_t next = cursor;
        for (size_t offset = 0; offset < bytes; offset += stride) {
            struct efi_memory_descriptor d;
            copy_bytes(&d, (const uint8_t *)map + offset, sizeof(d));
            uint64_t stop = d.physical_start + d.number_of_pages * 4096;
            if (d.physical_start <= cursor && stop > next)
                next = stop;
        }
        if (next == cursor)
            return 0;
        cursor = next;
    }
    return 1;
}

struct transition_tables {
    uint64_t base;
    size_t used;
};
static uint64_t *new_table(struct transition_tables *tables) {
    if (tables->used == TRANSITION_PT_PAGES)
        return NULL;
    return (uint64_t *)(uintptr_t)(tables->base + tables->used++ * 4096);
}
static int identity_range(struct transition_tables *tables, uint64_t base,
                          uint64_t bytes) {
    if (!bytes || base >= (1ULL << 47) || bytes > (1ULL << 47) - base)
        return -1;
    uint64_t end = base + bytes;
    uint64_t *root = (uint64_t *)(uintptr_t)tables->base;
    for (uint64_t address = base & ~0x1fffffULL; address < end;
         address += 0x200000) {
        uint64_t *table = root;
        for (unsigned shift = 39; shift >= 30; shift -= 9) {
            size_t index = (address >> shift) & 511;
            if (!table[index]) {
                uint64_t *next = new_table(tables);
                if (!next)
                    return -1;
                table[index] = (uint64_t)(uintptr_t)next | 3;
            }
            table = (uint64_t *)(uintptr_t)(table[index] & ~4095ULL);
        }
        table[(address >> 21) & 511] = address | 0x83; /* 2 MiB, supervisor */
    }
    return 0;
}
static uint64_t prepare_transition(uint64_t scratch, uint64_t image_base,
                                   uint64_t image_bytes) {
    /* A bounded PE image also bounds identity-map construction time. */
    if (!image_bytes || image_bytes > 0x80000000ULL ||
        image_bytes > UINT64_MAX - image_base)
        return 0;
    struct transition_tables tables = {scratch + TRANSITION_PT_OFFSET, 1};
    zero_bytes((void *)(uintptr_t)tables.base, TRANSITION_PT_PAGES * 4096u);
    if (identity_range(&tables, image_base, image_bytes) ||
        identity_range(&tables, KERNEL_LOAD_ADDRESS, KERNEL_MEMORY_BYTES) ||
        identity_range(&tables, scratch, SCRATCH_PAGES * 4096u))
        return 0;
    return tables.base;
}

/* Once this is entered no firmware call, old stack, or old page table is used. */
__attribute__((noreturn, noinline)) static void
enter_kernel(struct transition *t) {
    copy_bytes((void *)(uintptr_t)KERNEL_LOAD_ADDRESS,
               (const void *)(uintptr_t)t->source, t->file_bytes);
    zero_bytes((void *)(uintptr_t)(KERNEL_LOAD_ADDRESS + t->file_bytes),
               t->memory_bytes - t->file_bytes);
    __asm__ __volatile__("movq %0, %%rdi\n\t"
                         "movq %1, %%rsi\n\t"
                         "movq %2, %%rdx\n\t"
                         "jmp *%3\n\t"
                         :
                         : "r"(t->info), "r"(t->rsdp), "r"(t->framebuffer),
                           "r"((uint64_t)KERNEL_UEFI_ENTRY)
                         : "rdi", "rsi", "rdx", "memory");
    __builtin_unreachable();
}

/* ACPI is allowed in BootServicesData. Validate and protect the actual table
 * graph as well as EFI type 9/10 descriptors before reclaiming the destination.
 * Reads are bounded by the final memory map, not by untrusted table lengths. */
struct acpi_guard {
    const void *map;
    size_t bytes, stride;
};
static const uint8_t *guard_read(const struct acpi_guard *g, uint64_t base,
                                 size_t length) {
    if (!length || length > UINT64_MAX - base)
        return NULL;
    uint64_t end = base + length, cursor = base;
    while (cursor < end) {
        uint64_t next = cursor;
        for (size_t offset = 0; offset < g->bytes; offset += g->stride) {
            struct efi_memory_descriptor d;
            copy_bytes(&d, (const uint8_t *)g->map + offset, sizeof(d));
            uint64_t stop = d.physical_start + d.number_of_pages * 4096;
            if (((d.type >= 1 && d.type <= 7) || d.type == 9 || d.type == 10) &&
                d.physical_start <= cursor && stop > next)
                next = stop;
        }
        if (next == cursor)
            return NULL;
        cursor = next;
    }
    return (const uint8_t *)(uintptr_t)base;
}
static uint64_t read_number(const uint8_t *p, size_t bytes) {
    uint64_t n = 0;
    for (size_t i = 0; i < bytes; i++)
        n |= (uint64_t)p[i] << (8 * i);
    return n;
}
static int has_signature(const uint8_t *p, const char *signature,
                         size_t bytes) {
    for (size_t i = 0; i < bytes; i++)
        if (p[i] != (uint8_t)signature[i])
            return 0;
    return 1;
}
static int checksum_ok(const uint8_t *p, size_t bytes) {
    uint8_t sum = 0;
    for (size_t i = 0; i < bytes; i++)
        sum += p[i];
    return sum == 0;
}
static const uint8_t *guard_table(const struct acpi_guard *g, uint64_t pa,
                                  int facs, size_t *length) {
    const uint8_t *p = guard_read(g, pa, facs ? 8 : 36);
    if (!p || (facs && !has_signature(p, "FACS", 4)))
        return NULL;
    *length = read_number(p + 4, 4);
    if (*length < (facs ? 64u : 36u) || *length > 0x100000 ||
        overlaps(KERNEL_LOAD_ADDRESS, KERNEL_MEMORY_BYTES, pa, *length))
        return NULL;
    p = guard_read(g, pa, *length);
    if (!p || (!facs && !checksum_ok(p, *length)))
        return NULL;
    return p;
}
static int acpi_destination_safe(const void *map, size_t bytes, size_t stride,
                                 uint64_t rsdp) {
    if (!rsdp)
        return 1;
    struct acpi_guard g = {map, bytes, stride};
    const uint8_t *p = guard_read(&g, rsdp, 20);
    if (!p || !has_signature(p, "RSD PTR ", 8) || !checksum_ok(p, 20))
        return 0;
    uint64_t root = read_number(p + 16, 4);
    size_t step = 4, length = 20;
    if (p[15] >= 2) {
        p = guard_read(&g, rsdp, 36);
        if (!p)
            return 0;
        length = read_number(p + 20, 4);
        if (length < 36 || length > 4096)
            return 0;
        p = guard_read(&g, rsdp, length);
        if (!p || !checksum_ok(p, length))
            return 0;
        uint64_t xsdt = read_number(p + 24, 8);
        if (xsdt) {
            root = xsdt;
            step = 8;
        }
    }
    if (overlaps(KERNEL_LOAD_ADDRESS, KERNEL_MEMORY_BYTES, rsdp, length))
        return 0;
    p = guard_table(&g, root, 0, &length);
    if (!p || !has_signature(p, step == 8 ? "XSDT" : "RSDT", 4) ||
        (length - 36) % step || (length - 36) / step > 1024)
        return 0;
    size_t count = (length - 36) / step;
    for (size_t i = 0; i < count; i++) {
        uint64_t child = read_number(p + 36 + i * step, step);
        size_t child_length;
        const uint8_t *table = guard_table(&g, child, 0, &child_length);
        if (!table)
            return 0;
        if (has_signature(table, "FACP", 4)) {
            uint64_t facs = child_length >= 40 ? read_number(table + 36, 4) : 0;
            uint64_t dsdt = child_length >= 44 ? read_number(table + 40, 4) : 0;
            if (child_length >= 140 && read_number(table + 132, 8))
                facs = read_number(table + 132, 8);
            if (child_length >= 148 && read_number(table + 140, 8))
                dsdt = read_number(table + 140, 8);
            if (facs && !guard_table(&g, facs, 1, &child_length))
                return 0;
            if (dsdt) {
                const uint8_t *d = guard_table(&g, dsdt, 0, &child_length);
                if (!d || !has_signature(d, "DSDT", 4))
                    return 0;
            }
        }
    }
    return 1;
}

/* UEFI 2.10 section 12.9. The active mode is retained, including padded
 * scanlines. PixelBltOnly has no CPU-addressable framebuffer and is rejected. */
struct efi_gop_info {
    uint32_t version, width, height, format;
    uint32_t red_mask, green_mask, blue_mask, reserved_mask;
    uint32_t pixels_per_scanline;
};
struct efi_gop_mode {
    uint32_t max_mode, mode;
    struct efi_gop_info *info;
    efi_uintn_t info_bytes;
    uint64_t framebuffer_base;
    efi_uintn_t framebuffer_bytes;
};
struct efi_gop {
    void *query_mode, *set_mode, *blt;
    struct efi_gop_mode *mode;
};
static const struct efi_guid gop_guid = {
    0x9042a9de, 0x23dc, 0x4a38, {0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a}};
static int gop_mask_contiguous(uint32_t mask) {
    if (!mask) return 0;
    while (!(mask & 1)) mask >>= 1;
    return !(mask & (mask + 1));
}
static int describe_gop(const struct efi_gop_mode *mode,
                        boot_framebuffer_t *out) {
    zero_bytes(out, sizeof(*out));
    if (!mode || !mode->info || mode->info_bytes < sizeof(*mode->info) ||
        !mode->max_mode || mode->mode >= mode->max_mode) return -1;
    const struct efi_gop_info *i = mode->info;
    uint32_t r, g, b, reserved, bytes;
    if (i->version || i->format > 2) return -1;
    if (i->format < 2) {
        r = i->format == 0 ? 0xff : 0xff0000;
        g = 0xff00; b = i->format == 0 ? 0xff0000 : 0xff;
        reserved = 0xff000000; bytes = 4;
    } else {
        r = i->red_mask; g = i->green_mask; b = i->blue_mask;
        reserved = i->reserved_mask;
        if (!gop_mask_contiguous(r) || !gop_mask_contiguous(g) ||
            !gop_mask_contiguous(b) || (r & g) || (r & b) || (g & b) ||
            ((r | g | b) & reserved)) return -1;
        uint32_t all = r | g | b | reserved;
        unsigned bits = 0;
        while (all) { bits++; all >>= 1; }
        bytes = (bits + 7) / 8;
        if (bytes < 2 || bytes > 4) return -1;
    }
    uint64_t pitch = (uint64_t)i->pixels_per_scanline * bytes;
    uint64_t visible = pitch * i->height;
    if (!i->width || !i->height || i->width > 16384 || i->height > 16384 ||
        i->pixels_per_scanline < i->width || pitch > UINT32_MAX ||
        !mode->framebuffer_base || (mode->framebuffer_base & 4095) ||
        mode->framebuffer_bytes > 256ULL * 1024 * 1024 ||
        !mode->framebuffer_bytes ||
        mode->framebuffer_base > UINT64_MAX - mode->framebuffer_bytes ||
        ((visible + 4095) & ~4095ULL) > mode->framebuffer_bytes ||
        overlaps(KERNEL_LOAD_ADDRESS, KERNEL_MEMORY_BYTES,
                 mode->framebuffer_base, mode->framebuffer_bytes)) return -1;
    *out = (boot_framebuffer_t){BOOT_FRAMEBUFFER_MAGIC, BOOT_FRAMEBUFFER_VERSION,
        mode->framebuffer_base, mode->framebuffer_bytes, i->width, i->height,
        (uint32_t)pitch, bytes * 8, r, g, b, reserved};
    return 0;
}
static int collect_gop(struct efi_system_table *st, boot_framebuffer_t *out) {
    struct efi_gop *gop = NULL;
    efi_status_t status = st->boot_services->handle_protocol(
        st->console_out_handle, &gop_guid, (void **)&gop);
    if (EFI_ERROR(status) || !gop) {
        if (!st->boot_services->locate_protocol) return -1;
        status = st->boot_services->locate_protocol(&gop_guid, NULL, (void **)&gop);
    }
    return EFI_ERROR(status) || !gop ? -1 : describe_gop(gop->mode, out);
}

efi_status_t efi_main(efi_handle_t image, struct efi_system_table *st) {
    static efi_char16_t loading[] = {'A', '2', '0', 'O',  'S',  ':', ' ', 'U',
                                     'E', 'F', 'I', ':',  ' ',  'l', 'o', 'a',
                                     'd', 'i', 'n', 'g',  ' ',  'k', 'e', 'r',
                                     'n', 'e', 'l', '\r', '\n', 0};
    struct efi_boot_services *bs = st->boot_services;
    struct efi_loaded_image *loaded = NULL;
    size_t ksize = (size_t)(_binary_kernel_bin_end - _binary_kernel_bin_start);
    efi_uintn_t kernel_pages = ((uint64_t)KERNEL_MEMORY_BYTES + 4095) / 4096;
    efi_physical_address_t kernel_addr = KERNEL_LOAD_ADDRESS;
    efi_physical_address_t scratch = 0xffffffffULL;
    efi_uintn_t map_size, map_key, desc_size;
    uint32_t desc_version;
    efi_status_t status;
    const char *stage = "validate kernel span";
    uintptr_t acpi_rsdp = find_acpi_rsdp(st);
    uint32_t cmdline;
    uint32_t bootcount = 0;
    int exit_attempted = 0;
    if (!ksize || KERNEL_MEMORY_BYTES < ksize ||
        KERNEL_MEMORY_BYTES > 0x80000000ULL - KERNEL_LOAD_ADDRESS) {
        status = 0x8000000000000002ULL;
        goto failed;
    }
    print(st, loading);
    stage = "LoadedImage protocol";
    status = bs->handle_protocol(image, &loaded_image_guid, (void **)&loaded);
    if (EFI_ERROR(status))
        goto failed;
    /* Establish the ESP breadcrumb before any check that can fail.  A machine
     * whose panel never lights up still leaves this file on the stick. */
    esp_open(st, image);
    bootcount = esp_read_bootcount() + 1U;
    esp_write_bootcount(bootcount);
    early_open(bootcount);
    esp_log_stage("loader");
    esp_log_text("bootcount=");
    {
        char digits[16];
        size_t d = 0;
        uint32_t v = bootcount;
        if (!v) digits[d++] = '0';
        while (v) { digits[d++] = (char)('0' + (v % 10)); v /= 10; }
        char out[17]; size_t n = 0;
        while (d) out[n++] = digits[--d];
        out[n] = '\0';
        esp_log_text(out);
    }
    esp_log_text("\n");
    stage = "validate loaded image";
    status = 0x8000000000000002ULL;
    if (!loaded || !loaded->image_size ||
        loaded->image_size > UINT64_MAX - (uintptr_t)loaded->image_base ||
        (uintptr_t)_binary_kernel_bin_start < (uintptr_t)loaded->image_base ||
        (uintptr_t)_binary_kernel_bin_end >
            (uintptr_t)loaded->image_base + loaded->image_size ||
        overlaps(KERNEL_LOAD_ADDRESS, kernel_pages * 4096,
                 (uintptr_t)loaded->image_base, loaded->image_size))
        goto failed;
    uint64_t image_base = (uintptr_t)loaded->image_base,
             image_bytes = loaded->image_size;
    uint64_t cr4;
    __asm__ __volatile__("mov %%cr4, %0" : "=r"(cr4));
    stage = "five-level paging unsupported";
    if (cr4 & (1ULL << 12))
        goto failed;

    /* Prefer an explicit allocation, but fragmented BootServicesData may own
     * part of the fixed BSS destination. In that case defer ALL writes until
     * the final map proves it reclaimable and ExitBootServices succeeds. */
    stage = "allocate kernel";
    status = bs->allocate_pages(EFI_ALLOCATE_ADDRESS, EFI_LOADER_DATA,
                                kernel_pages, &kernel_addr);
    if (EFI_ERROR(status)) {
        if (status != 0x800000000000000eULL && status != 0x8000000000000009ULL)
            goto failed;
        report_failure(st, "defer fixed kernel until ExitBootServices", status);
    } else if (kernel_addr != KERNEL_LOAD_ADDRESS) {
        status = 0x8000000000000002ULL;
        goto failed;
    }
    esp_log_stage("scratch");
    stage = "allocate scratch";
    status = bs->allocate_pages(EFI_ALLOCATE_MAX_ADDRESS, EFI_LOADER_DATA,
                                SCRATCH_PAGES, &scratch);
    if (EFI_ERROR(status))
        goto failed;
    stage = "validate scratch";
    status = 0x8000000000000002ULL;
    if ((scratch & 4095) ||
        scratch > 0x100000000ULL - SCRATCH_PAGES * 4096ULL ||
        overlaps(KERNEL_LOAD_ADDRESS, kernel_pages * 4096, scratch,
                 SCRATCH_PAGES * 4096ULL) ||
        overlaps(image_base, image_bytes, scratch, SCRATCH_PAGES * 4096ULL))
        goto failed;
    stage = "prepare private page tables";
    uint64_t new_cr3 = prepare_transition(scratch, image_base, image_bytes);
    if (!new_cr3)
        goto failed;
    struct transition *handoff =
        (struct transition *)(uintptr_t)(scratch + TRANSITION_OFFSET);
    *handoff = (struct transition){(uintptr_t)_binary_kernel_bin_start, ksize,
                                   KERNEL_MEMORY_BYTES,
                                   scratch + MB_INFO_OFFSET, acpi_rsdp, 0};
    boot_framebuffer_t *scanout = (boot_framebuffer_t *)(uintptr_t)
        (scratch + TRANSITION_OFFSET + 0x100);
    if (!collect_gop(st, scanout))
        handoff->framebuffer = (uintptr_t)scanout;
    uint64_t stack = scratch + SCRATCH_PAGES * 4096ULL - 40;
    /* A synthetic return slot and 32-byte MS ABI shadow area fit in scratch. */
    *(uint64_t *)(uintptr_t)stack = 0;
    cmdline = collect_load_options(st, image, scratch);
    append_cmdline_bootcount(cmdline, bootcount);

    esp_log_stage("memory-map");
    /* Validate the destination once while firmware diagnostics are still
     * available. Then finish file I/O before taking the actual exit map.
     * Opening, flushing and closing even the ESP log can change its key. */
    for (int pass = 0; pass < 3; pass++) {
        stage = "GetMemoryMap";
        map_size = EFI_MAP_BYTES;
        status = bs->get_memory_map(&map_size, (void *)(uintptr_t)scratch,
                                    &map_key, &desc_size, &desc_version);
        if (EFI_ERROR(status))
            break;
        if (map_size > EFI_MAP_BYTES) {
            status = 0x8000000000000002ULL;
            break;
        }
        void *map = (void *)(uintptr_t)scratch;
        uint64_t info_base = scratch + MB_INFO_OFFSET;
        struct mb_mmap_entry *out =
            (struct mb_mmap_entry *)(uintptr_t)(info_base +
                                                sizeof(struct mb_info));
        size_t entries = 0;
        stage = "export memory map";
        int converted = export_memory_map(
            map, map_size, desc_size, desc_version, scratch,
            SCRATCH_PAGES * 4096ULL, out, MB_INFO_MAX_ENTRIES, &entries);
        if (converted) {
            status =
                converted == -2 ? 0x8000000000000009ULL : 0x8000000000000002ULL;
            break;
        }
        stage = "kernel destination is reserved or missing";
        status = 0x8000000000000002ULL;
        if (!target_reclaimable(map, map_size, desc_size, KERNEL_LOAD_ADDRESS,
                                kernel_pages * 4096)) {
            for (size_t o = 0; pass == 0 && o < map_size; o += desc_size) {
                struct efi_memory_descriptor d;
                copy_bytes(&d, (const uint8_t *)map + o, sizeof(d));
                if (!overlaps(KERNEL_LOAD_ADDRESS, kernel_pages * 4096,
                              d.physical_start, d.number_of_pages * 4096))
                    continue;
                report_failure(st, "destination descriptor type", d.type);
                report_failure(st, "destination descriptor start",
                               d.physical_start);
                report_failure(st, "destination descriptor pages",
                               d.number_of_pages);
                report_failure(st, "destination descriptor attributes",
                               d.attribute);
            }
            break;
        }
        stage = "kernel destination overlaps or invalid ACPI tables";
        if (!acpi_destination_safe(map, map_size, desc_size, acpi_rsdp))
            break;
        struct mb_info *mi = (struct mb_info *)(uintptr_t)info_base;
        zero_bytes(mi, sizeof(*mi));
        mi->flags = MB_INFO_MEM_MAP | (cmdline ? MB_INFO_CMDLINE : 0);
        mi->mmap_addr = (uint32_t)(info_base + sizeof(struct mb_info));
        mi->mmap_length = (uint32_t)(entries * sizeof(struct mb_mmap_entry));
        mi->cmdline = cmdline;
        if (pass == 0) {
            esp_log_stage("destination");
            esp_log_stage("exit-boot-services");
            esp_log_text("RESULT handoff=attempt\n");
            early_close();
            if (g_esp_root) {
                (void)g_esp_root->close(g_esp_root);
                g_esp_root = NULL;
                g_esp_ready = 0;
            }
            continue;
        }
        stage = "ExitBootServices";
        /* If the firmware partially shuts down on the first attempt, only
         * refresh the map and retry. Console and filesystem calls are unsafe. */
        exit_attempted = 1;
        status = bs->exit_boot_services(image, map_key);
        if (!EFI_ERROR(status)) {
            /* Preload every operand before dropping the firmware's stack and
             * CR3. No firmware memory can be overwritten before this point. */
            __asm__ __volatile__("cli\n\tcld\n\t"
                                 "movq %0, %%cr3\n\t"
                                 "movq %1, %%rsp\n\t"
                                 "movq %2, %%rcx\n\t"
                                 "jmp *%3\n\t"
                                 :
                                 : "r"(new_cr3), "r"(stack), "r"(handoff),
                                   "r"(enter_kernel)
                                 : "rcx", "memory");
            __builtin_unreachable();
        }
        if (status != 0x8000000000000002ULL)
            break;
    }
failed:
    if (!exit_attempted) {
        report_failure(st, stage, status);
        early_close();
    }
    return status;
}
