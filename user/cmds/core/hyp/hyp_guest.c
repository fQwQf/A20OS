/*
 * Shared guest loader for /hyp_boot and /hypvm -- see hyp_guest.h for what
 * this file is and why it exists.  The code below is the loader those two
 * programs used to carry separately: the ELF walk, the minimal FDT, and the
 * syscall wrappers.  The comments on the awkward parts (the LMA vs e_entry
 * translation, the .bss tail, why the DTB has to be identity-reachable) are
 * load-bearing explanations of the *kernel* side, so they moved with the code
 * rather than being rewritten.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "hyp_guest.h"

/* Mirror of kernel/include/core/syscall_nr.h, hand-copied the way hyp_test.c
 * does because musl has no header for the A20OS extensions. */
#define SYS_hyp_vm_create      907
#define SYS_hyp_vm_load        908
#define SYS_hyp_vcpu_create    909
#define SYS_hyp_vcpu_run       910
#define SYS_hyp_vm_destroy     911
#define SYS_hyp_vcpu_set_boot  912
#define SYS_hyp_vm_set_marker  913
#define SYS_hyp_vm_status      914

/* Source for the zero-fill tail of every PT_LOAD segment ([p_filesz, p_memsz)
 * is .bss).  The host's hyp_vm_load() copies from this pointer and zeroes the
 * rest of the final page itself, so it only has to be readable and really
 * zero -- it is never written. */
static const unsigned char zero_page[HYP_GUEST_PAGE_SIZE];

const char *hyp_guest_exit_name(long exit_reason)
{
    switch (exit_reason) {
    case HYP_GUEST_EXIT_SHUTDOWN: return "shutdown";
    case HYP_GUEST_EXIT_FAULT:    return "fault";
    case HYP_GUEST_EXIT_ERROR:    return "error";
    default:                      return "none";
    }
}

/* ------------------------------------------------------------------ syscalls */

long hyp_guest_vm_create(uint64_t mem_size)
{
    return syscall(SYS_hyp_vm_create, (unsigned long)mem_size);
}

int hyp_guest_vm_load(long vm, uint64_t gpa, const void *src, uint64_t len)
{
    return (int)syscall(SYS_hyp_vm_load, vm, (unsigned long)gpa,
                        (unsigned long)src, (unsigned long)len);
}

long hyp_guest_vcpu_create(long vm, uint64_t entry_gpa)
{
    return syscall(SYS_hyp_vcpu_create, vm, (unsigned long)entry_gpa);
}

int hyp_guest_vcpu_set_boot(long vcpu, uint64_t hartid, uint64_t dtb_gpa)
{
    return (int)syscall(SYS_hyp_vcpu_set_boot, vcpu, (unsigned long)hartid,
                        (unsigned long)dtb_gpa);
}

int hyp_guest_vm_set_marker(long vm, const char *marker)
{
    return (int)syscall(SYS_hyp_vm_set_marker, vm, (unsigned long)marker);
}

long hyp_guest_vcpu_run(long vcpu)
{
    return syscall(SYS_hyp_vcpu_run, vcpu);
}

long hyp_guest_vm_status(long vm, struct hyp_guest_status *st)
{
    return syscall(SYS_hyp_vm_status, vm, (unsigned long)st);
}

int hyp_guest_vm_destroy(long vm)
{
    return (int)syscall(SYS_hyp_vm_destroy, vm);
}

const char *hyp_guest_call_error(long rc)
{
    if (rc >= 0)
        return NULL;
    /* The bridge answers hyp_supported() == 0 with -EOPNOTSUPP
     * (kernel/abi/linux/sys_a20_bridge.c:285), which is ENOTSUP to a libc.  A
     * kernel built without the hyp calls at all answers ENOSYS.  Both mean
     * "nothing was measured", which has to be said rather than folded into a
     * generic failure: this is the case where a boot gate goes green having
     * tested nothing (see docs/hypervisor/01-a20os-guest.md §8.4). */
    if (errno == ENOTSUP)
        return "kernel refuses the hyp syscalls: this CPU has no H extension "
               "(QEMU needs -cpu rv64,h=true)";
    if (errno == ENOSYS)
        return "kernel has no hyp syscalls (built without CONFIG_HYPER?)";
    return NULL;
}

/* ------------------------------------------------------------------ files */

unsigned char *hyp_guest_read_file(const char *path, size_t *out_len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;

    struct stat sb;
    if (fstat(fd, &sb) < 0 || sb.st_size <= 0) {
        close(fd);
        return NULL;
    }

    unsigned char *buf = malloc((size_t)sb.st_size);
    if (!buf) {
        close(fd);
        return NULL;
    }

    size_t done = 0;
    while (done < (size_t)sb.st_size) {
        ssize_t n = read(fd, buf + done, (size_t)sb.st_size - done);
        if (n <= 0) {
            free(buf);
            close(fd);
            return NULL;
        }
        done += (size_t)n;
    }
    close(fd);
    *out_len = done;
    return buf;
}

/* ------------------------------------------------------------------ ELF64 */

#define PT_LOAD    1
#define EM_RISCV   243
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ET_EXEC    2

struct elf_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
};

/* Every PT_LOAD, in the order the file lists them.  Loading in link order
 * matters only for the diagnostics; the addresses are absolute. */
#define MAX_PHDR 16

int hyp_guest_load_elf(long vm, const unsigned char *img, size_t size,
                       uint64_t *entry_gpa, uint64_t *image_end)
{
    if (size < 64 || memcmp(img, "\177ELF", 4) != 0)
        return -1;
    if (img[4] != ELFCLASS64 || img[5] != ELFDATA2LSB)
        return -1;

    uint16_t e_type, e_machine;
    uint64_t e_entry, e_phoff;
    uint16_t e_phentsize, e_phnum;
    memcpy(&e_type, img + 16, sizeof(e_type));
    memcpy(&e_machine, img + 18, sizeof(e_machine));
    memcpy(&e_entry, img + 24, sizeof(e_entry));
    memcpy(&e_phoff, img + 32, sizeof(e_phoff));
    memcpy(&e_phentsize, img + 54, sizeof(e_phentsize));
    memcpy(&e_phnum, img + 56, sizeof(e_phnum));

    if (e_type != ET_EXEC || e_machine != EM_RISCV)
        return -1;
    if (e_phentsize < sizeof(struct elf_phdr) || e_phnum == 0 || e_phnum > MAX_PHDR)
        return -1;
    if (e_phoff + (uint64_t)e_phnum * e_phentsize > size)
        return -1;

    *entry_gpa = e_entry;
    *image_end = 0;

    int nloaded = 0;
    for (unsigned i = 0; i < e_phnum; i++) {
        struct elf_phdr ph;
        memcpy(&ph, img + e_phoff + (uint64_t)i * e_phentsize, sizeof(ph));
        if (ph.p_type != PT_LOAD)
            continue;
        if (ph.p_filesz && ph.p_offset + ph.p_filesz > size)
            return -1;

        uint64_t end = ph.p_paddr + ph.p_memsz;
        if (end > *image_end)
            *image_end = end;

        /* e_entry is a virtual address; the guest is entered at the LMA. */
        if (e_entry >= ph.p_vaddr && e_entry < ph.p_vaddr + ph.p_memsz)
            *entry_gpa = ph.p_paddr + (e_entry - ph.p_vaddr);

        /* A segment whose LMA is not page-aligned has to share its first page
         * with whatever precedes it, or the loader writes the head of the
         * segment over the wrong bytes.  The A20OS kernel's three segments are
         * all aligned (readelf: 0x80200000/0x804cf000/0x8050d000), so this is
         * the general case rather than a live one. */
        uint64_t skew = ph.p_paddr & (HYP_GUEST_PAGE_SIZE - 1);
        if (skew && skew > ph.p_offset)
            return -1;

        uint64_t done = 0;
        uint64_t len  = ph.p_filesz;
        const unsigned char *src = img + ph.p_offset - skew;
        while (done < len) {
            /* One call per megabyte: the bridge caps a single load at 16 MiB
             * and stages it a page at a time, so smaller calls keep the host's
             * staging cost proportional to what is actually loaded. */
            uint64_t chunk = len - done;
            if (chunk > (1ULL << 20))
                chunk = 1ULL << 20;
            if (hyp_guest_vm_load(vm, ph.p_paddr + done, src + done, chunk) < 0)
                return -1;
            done += chunk;
        }

        /* The zero-fill tail: [p_filesz, p_memsz) is .bss, and it is part of the
         * image even though no file byte covers it.  It has to be MAPPED here,
         * not left to the host's demand-fill, for two reasons that are both
         * about the guest's first instructions rather than about bookkeeping:
         *
         *   - entry.S runs `la sp,_stack_end` and then clears .bss, and
         *     _stack_end is inside .bss.  With the tail unmapped, the very
         *     first store of the very first guest instruction faults on a page
         *     the second stage has never seen -- roughly 2800 pages of them,
         *     one trap each, before the guest has printed anything.
         *   - A20OS sets sp into that same .bss before it is mapped, so a
         *     demand-fill that hands the guest a zero page at sp does not help:
         *     the guest's stack would still be the demand-filled page rather
         *     than the image's, and the host would keep refilling on the way
         *     down.
         *
         * Only whole pages strictly past the file-backed ones are mapped, so this
         * never re-touches a page the loop above already mapped (hyp_vm_load
         * refuses a page that is already in the table, and would fail the whole
         * load).  hyp_ram_fill stays in the kernel as the general fallback for
         * pages the image never mentions; it is not what makes .bss work.
         */
        uint64_t file_end = ph.p_paddr + ph.p_filesz;
        uint64_t mem_end  = ph.p_paddr + ph.p_memsz;
        uint64_t tail     = (file_end + HYP_GUEST_PAGE_SIZE - 1) &
                            ~(uint64_t)(HYP_GUEST_PAGE_SIZE - 1);
        while (tail < mem_end) {
            /* ONE PAGE per call, and not the 1 MiB chunk the file loop uses:
             * the source is zero_page, which is one page long, and the bridge
             * copies len bytes out of userspace in PAGE_SIZE steps
             * (kernel/abi/linux/sys_a20_bridge.c:347-352).  A 1 MiB len would
             * have the kernel read up to 1 MiB from a 4 KiB object -- past its
             * end, straight into whatever follows it in the task's image.  The
             * cost of one page per call is only the syscall: the bridge
             * already loops per page, so the number of host-side copies is the
             * same either way.  PAGE_SIZE divides evenly, so tail stays
             * page-aligned and the last call covers mem_end's partial page. */
            uint64_t chunk = mem_end - tail;
            if (chunk > HYP_GUEST_PAGE_SIZE)
                chunk = HYP_GUEST_PAGE_SIZE;
            if (hyp_guest_vm_load(vm, tail, zero_page, chunk) < 0)
                return -1;
            tail += chunk;
        }

        nloaded++;
    }
    return nloaded;
}

/* -------------------------------------------------------------------- FDT */

/*
 * The minimum device tree an A20OS guest needs, established by reading the
 * three parsers that run before the kernel prints anything useful:
 *
 *   /chosen/bootargs   fdt_extract_bootargs() (kernel/core/fdt_bootargs.c)
 *                      scans for a depth-2 node named "chosen"; absent, the
 *                      kernel simply has no cmdline (arch_bootargs_get falls
 *                      through to NULL), so this is for telling guest output
 *                      from host output, not for booting.
 *   /memory@.../reg    riscv64_memory_init() (kernel/arch/riscv64/platform/
 *                      fdt.c) narrows the board's 1 GiB window to this.  It
 *                      must be present and must match the stage-2 RAM window:
 *                      without it the guest's allocator believes in RAM the
 *                      stage-2 will not translate, and the first allocation
 *                      past the window is a fatal guest fault.
 *   /cpus/timebase-frequency
 *                      riscv64_fdt_timebase_freq() falls back to the arch
 *                      constant (10 MHz on qemu-virt), so this only pins what
 *                      the fallback already is.
 *   /cpus/riscv,isa    "sstc" here is what keeps the guest off the SBI timer
 *                      call: timer_set_interval() uses stimecmp when
 *                      riscv64_fdt_has_isa_extension("sstc") is true and
 *                      firmware_set_timer() otherwise, and the run loop's SBI
 *                      subset is putchar and shutdown only (hyp_vcpu.h), so a
 *                      guest that asks for a timer the host does not implement
 *                      dies at [INIT] Timer initialized.
 *
 * Nothing else is read before the banner: the console is a fixed address
 * (UART0_BASE), the virtio-mmio slots are scanned at fixed addresses
 * (virtio_mmio_enumerate), and there is no hart enumeration -- the vcpu slice
 * runs one hart, and hart 0 is exactly what entry.S's QEMU lottery handoff
 * skips (beqz a0 at entry.S:44).
 */

#define FDT_MAGIC        0xd00dfeedU
#define FDT_BEGIN_NODE   1U
#define FDT_END_NODE     2U
#define FDT_PROP         3U
#define FDT_END          9U
#define FDT_VERSION      17
#define FDT_LAST_COMP    16
#define FDT_HEADER_SIZE  40

struct blob {
    unsigned char *p;
    size_t len;
    size_t cap;
};

static int blob_put(struct blob *b, const void *src, size_t n)
{
    if (b->len + n > b->cap)
        return -1;
    memcpy(b->p + b->len, src, n);
    b->len += n;
    return 0;
}

static int blob_u32(struct blob *b, uint32_t v)
{
    /* Every FDT integer is big-endian, on a little-endian host included. */
    unsigned char t[4] = { (unsigned char)(v >> 24), (unsigned char)(v >> 16),
                           (unsigned char)(v >> 8), (unsigned char)v };
    return blob_put(b, t, 4);
}

/* Node names are NUL-terminated and padded to a 4-byte boundary. */
static int blob_name(struct blob *b, const char *name)
{
    size_t n = strlen(name) + 1;
    if (blob_put(b, name, n) < 0)
        return -1;
    /* One zero BYTE at a time.  blob_u32() writes 4 bytes, and 4 does not
 * change len & 3, so padding with it spins until blob_put() overflows the
 * cap -- which is exactly what it used to do here, misaligning the whole
 * struct block past the root node and leaving the guest's fdt.c reader
 * desynchronised at offset 0x44. */
    while (b->len & 3) {
        if (blob_put(b, "\0", 1) < 0)
            return -1;
    }
    return 0;
}

/* A property value: the raw bytes, padded to 4 with zeroes. */
static int blob_value(struct blob *b, const void *v, size_t n)
{
    if (blob_put(b, v, n) < 0)
        return -1;
    while (b->len & 3) {
        if (blob_put(b, "\0", 1) < 0)
            return -1;
    }
    return 0;
}

static void blob_pad8(struct blob *b)
{
    static const unsigned char z[8] = { 0 };
    blob_put(b, z, (8 - (b->len & 7)) & 7);
}

/* Append to the strings block, returning the name offset.  Offset 0 is the
 * empty string every block starts with. */
static uint32_t blob_str(struct blob *b, const char *name)
{
    uint32_t off = (uint32_t)b->len;
    blob_put(b, name, strlen(name) + 1);
    return off;
}

size_t hyp_guest_fdt_capacity(void)
{
    /* The blob is a few hundred bytes; this is its upper bound with room to
     * spare, and the caller checks the build's return against it. */
    return 2048;
}

size_t hyp_guest_fdt_build(unsigned char *out, size_t cap,
                           uint64_t mem_base, uint64_t mem_size,
                           const char *bootargs)
{
    size_t cap_st = cap / 2;          /* struct block gets half */
    size_t cap_sr = cap - cap_st;     /* strings block the rest */
    struct blob st = { malloc(cap_st), 0, cap_st };
    struct blob sr = { malloc(cap_sr), 0, cap_sr };
    if (!st.p || !sr.p) {
        free(st.p);
        free(sr.p);
        return 0;
    }
    sr.len = 1;
    sr.p[0] = '\0';      /* strings block offset 0 IS the empty string */

    if (!bootargs)
        bootargs = "";
    const char *isa   = "rv64imafdch_sstc";
    uint32_t timebase = 10000000;   /* qemu-virt aclin-mtimer */
    char memname[32];

    snprintf(memname, sizeof(memname), "memory@%llx",
             (unsigned long long)mem_base);

    blob_u32(&st, FDT_BEGIN_NODE); blob_name(&st, "");        /* root */
    blob_u32(&st, FDT_BEGIN_NODE); blob_name(&st, "chosen");
    blob_u32(&st, FDT_PROP);
    blob_u32(&st, (uint32_t)strlen(bootargs) + 1);
    blob_u32(&st, blob_str(&sr, "bootargs"));
    blob_value(&st, bootargs, strlen(bootargs) + 1);
    blob_u32(&st, FDT_END_NODE);

    blob_u32(&st, FDT_BEGIN_NODE); blob_name(&st, memname);
    blob_u32(&st, FDT_PROP);
    blob_u32(&st, 16);
    blob_u32(&st, blob_str(&sr, "reg"));
    blob_u32(&st, (uint32_t)(mem_base >> 32));
    blob_u32(&st, (uint32_t)mem_base);
    blob_u32(&st, (uint32_t)(mem_size >> 32));
    blob_u32(&st, (uint32_t)mem_size);
    blob_u32(&st, FDT_END_NODE);

    blob_u32(&st, FDT_BEGIN_NODE); blob_name(&st, "cpus");
    blob_u32(&st, FDT_PROP);
    blob_u32(&st, 4);
    blob_u32(&st, blob_str(&sr, "timebase-frequency"));
    blob_u32(&st, timebase);            /* already 4-aligned, no padding */
    blob_u32(&st, FDT_PROP);
    blob_u32(&st, (uint32_t)strlen(isa) + 1);
    blob_u32(&st, blob_str(&sr, "riscv,isa"));
    blob_value(&st, isa, strlen(isa) + 1);
    blob_u32(&st, FDT_END_NODE);

    blob_u32(&st, FDT_END_NODE);   /* root */
    blob_u32(&st, FDT_END);
    blob_pad8(&st);
    blob_pad8(&sr);

    /* 8-byte alignment: the reserve map and the struct block both want it. */
    size_t off_struct = FDT_HEADER_SIZE + 16;
    size_t off_strings = (off_struct + st.len + 7) & ~(size_t)7;
    size_t total       = off_strings + sr.len;
    total = (total + 7) & ~(size_t)7;
    if (total > cap)
        goto fail;

    unsigned char *p = out;
    memset(p, 0, total);
    uint64_t rsv[2] = { 0, 0 };   /* one terminator entry: no reserved ranges */
    p += FDT_HEADER_SIZE;
    memcpy(p, rsv, sizeof(rsv));
    p = out + off_struct;
    memcpy(p, st.p, st.len);
    p = out + off_strings;
    memcpy(p, sr.p, sr.len);

    /* The header last: it carries the offsets computed above. */
    uint32_t hdr[10] = {
        FDT_MAGIC, (uint32_t)total, (uint32_t)off_struct, (uint32_t)off_strings,
        FDT_HEADER_SIZE, FDT_VERSION, FDT_LAST_COMP, 0,
        (uint32_t)sr.len, (uint32_t)st.len,
    };
    for (int i = 0; i < 10; i++) {
        out[i * 4 + 0] = (unsigned char)(hdr[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(hdr[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(hdr[i] >> 8);
        out[i * 4 + 3] = (unsigned char)hdr[i];
    }

    free(st.p);
    free(sr.p);
    return total;

fail:
    free(st.p);
    free(sr.p);
    return 0;
}