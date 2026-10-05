/*
 * hyp_boot -- boot a real A20OS kernel as a hypervisor guest.
 *
 * The vcpu slice's own gate (hyp_test.c) runs 56 bytes of hand-assembled SBI
 * calls.  This one boots the same kernel the host is running: it reads the
 * guest ELF out of the host filesystem, lays its PT_LOAD segments into guest
 * RAM at their link-time physical addresses, hands the vcpu a minimal device
 * tree, and lets it run until it leaves.
 *
 * PASS is "the guest reached its own banner", not "the guest booted": a guest
 * with no disk cannot mount a rootfs and dies in init_kthread, which is a real
 * arrival, not a failure.  What would be a failure is the guest never printing
 * -- indistinguishable, on one shared console, from a host that has quietly
 * stopped.  So the verdict is taken from marker_seen, which the device model
 * counts over guest console bytes only (hyp_vcpu.h), and never from parsing
 * interleaved log text.
 *
 * Why the ELF needs translating: the kernel is linked higher-half
 * (VIRT_BASE 0xFFFFFFC080200000, ldscript.ld) with every segment's LMA 1:1
 * below it, so e_entry is a virtual address that is not a guest physical one.
 * QEMU's -kernel loader jumps to the LMA, and so does this: entry_gpa is
 * e_entry translated through the program headers.
 *
 * The calls go through the Linux ABI bridge (kernel/abi/linux/sys_a20_bridge.c,
 * numbers from kernel/include/core/syscall_nr.h), for the reason hyp_test.c
 * gives: a Linux-ABI task has no Native handle table to name a VM with.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

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

#define PAGE_SIZE      4096UL

/* Source for the zero-fill tail of every PT_LOAD segment ([p_filesz, p_memsz)
 * is .bss).  The host's hyp_vm_load() copies from this pointer and zeroes the
 * rest of the final page itself, so it only has to be readable and really
 * zero -- it is never written. */
static const unsigned char zero_page[PAGE_SIZE];

/* The FAT32 utilities image is mounted at /bin in the default (non
 * EXTERNAL_ROOT) boot -- kernel/fs/mount_setup.c mount_block_devices() picks
 * utilities_path = "/bin".  The guest ELF is written at the image root
 * (::/boot/guest-kernel.elf, tools/img.py), which in the running system is
 * /bin/boot/guest-kernel.elf, the same convention the other user-space tests
 * use for image-root paths. */
#define GUEST_ELF_PATH "/bin/boot/guest-kernel.elf"

/* Stage-2 RAM window default, hyp_vcpu.h: [0x80000000, base + mem_size).  The
 * kernel image (text+rodata+data+bss) ends around 0x810A0914, so 128 MiB holds
 * the image, the boot page tables and the stacks without pretending the guest
 * owns host memory it will never touch. */
#define GUEST_BASE 0x80000000ULL
#define GUEST_MEM  (128ULL << 20)

/* Where the loader's own FDT goes.  Two constraints, neither visible in the
 * code below:
 *
 *  - clear of the image (which reaches ~0x810D08EC), because entry.S clears
 *    BSS itself with `la` stores before it reads a1, and that would wipe a DTB
 *    parked inside the image;
 *  - inside the window entry.S identity-maps (BOOT_MAP_PHYS = 0x80000000,
 *    sixteen 1 GiB slots, entry.S:132).  a1 is a physical address by the SBI
 *    convention, and __boot_dtb_ptr is dereferenced as a plain pointer by
 *    riscv64_memory_init() and fdt_extract_bootargs() -- after the guest has
 *    installed its own satp.  A GPA is only still dereferenceable after that
 *    if the guest's identity map covers it, so a DTB outside the boot identity
 *    window faults the guest before it prints anything.
 */
#define GUEST_DTB_GPA 0x87f00000ULL

/* kernel/main.c prints "    A20OS Kernel \n" as the first thing kernel_main
 * does, before uart_init and long before any SBI call, and the guest writes it
 * to the 16550 at 0x10000000 directly (arch_uart_putc), so it is counted by the
 * device model's console byte counter.  A prefix of the host's own identical
 * banner cannot be confused with it: the marker is scanned over guest bytes
 * only.  Log evidence: .kernel-build/smoke/mm-stress-riscv64.log line 66. */
#define GUEST_MARKER "A20OS Kernel"

/* hyp_exit_reason_t, kernel/include/hyp/hyp_vcpu.h. */
#define HYP_EXIT_NONE     0
#define HYP_EXIT_SHUTDOWN 1
#define HYP_EXIT_FAULT    2
#define HYP_EXIT_ERROR    3

/* Mirror of struct hyp_vm_status in kernel/abi/linux/sys_a20_bridge.c. */
struct hyp_status {
    uint64_t exit;
    uint64_t scause;
    uint64_t stval;
    uint64_t htval;
    uint64_t marker_seen;
    uint64_t console_bytes;
};

static int fail(const char *what, long rc)
{
    printf("HYP_A20OS: FAIL %s (rc=%ld errno=%d)\n", what, rc, errno);
    return 1;
}

static int failf(const char *fmt, ...)
{
    va_list ap;
    printf("HYP_A20OS: FAIL ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    return 1;
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

static long load_elf(long vm, const unsigned char *img, size_t size,
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
        uint64_t skew = ph.p_paddr & (PAGE_SIZE - 1);
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
            long r = syscall(SYS_hyp_vm_load, vm,
                             (unsigned long)(ph.p_paddr + done),
                             (unsigned long)(src + done), (unsigned long)chunk);
            if (r < 0)
                return -1;
            done += chunk;
        }

        /* The zero-fill tail: [p_filesz, p_memsz) is .bss, and it is part of
         * the image even though no file byte covers it.  It has to be MAPPED
         * here, not left to the host's demand-fill, for two reasons that are
         * both about the guest's first instructions rather than about
         * bookkeeping:
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
         * Only whole pages strictly past the file-backed ones are mapped, so
         * this never re-touches a page the loop above already mapped (hyp_vm_load
         * refuses a page that is already in the table, and would fail the whole
         * load).  hyp_ram_fill stays in the kernel as the general fallback for
         * pages the image never mentions; it is not what makes .bss work.
         */
        uint64_t file_end = ph.p_paddr + ph.p_filesz;
        uint64_t mem_end  = ph.p_paddr + ph.p_memsz;
        uint64_t tail     = (file_end + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
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
            if (chunk > PAGE_SIZE)
                chunk = PAGE_SIZE;
            long r = syscall(SYS_hyp_vm_load, vm, (unsigned long)tail,
                             (unsigned long)zero_page, (unsigned long)chunk);
            if (r < 0)
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
 *                      through to NULL), so this is for distinguishing guest
 *                      output from host output, not for booting.
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
 *                      subset is putchar and shutdown only
 *                      (hyp_vcpu.h), so a guest that asks for a timer the host
 *                      does not implement dies at [INIT] Timer initialized.
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
    while (b->len & 3)
        return blob_u32(b, 0);
    return 0;
}

/* A property value: the raw bytes, padded to 4 with zeroes. */
static int blob_value(struct blob *b, const void *v, size_t n)
{
    if (blob_put(b, v, n) < 0)
        return -1;
    while (b->len & 3)
        return blob_u32(b, 0);
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

/* Build the whole blob into out (which must hold fdt_build_size() bytes). */
static size_t fdt_build(unsigned char *out, uint64_t mem_base, uint64_t mem_size)
{
    struct blob st = { malloc(1024), 0, 1024 };
    struct blob sr = { malloc(256), 0, 256 };
    if (!st.p || !sr.p)
        return 0;
    sr.len = 1;
    st.p[0] = '\0';

    const char *bootargs = "a20.hypguest=1";
    const char *isa      = "rv64imafdch_sstc";
    uint32_t timebase    = 10000000;   /* qemu-virt aclint-mtimer */
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
}

/* The blob is a few hundred bytes; this is its upper bound with room to
 * spare, and the caller checks the return against it. */
static size_t fdt_build_size(void) { return 2048; }

/* ------------------------------------------------------------------ main */

static unsigned char *read_whole(const char *path, size_t *out_len)
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

int main(void)
{
    unsigned char *dtb = malloc(fdt_build_size());
    if (!dtb)
        return fail("dtb alloc", -1);

    size_t dtb_len = fdt_build(dtb, GUEST_BASE, GUEST_MEM);
    if (!dtb_len)
        return fail("dtb build", -1);

    size_t img_len = 0;
    unsigned char *img = read_whole(GUEST_ELF_PATH, &img_len);
    if (!img)
        return failf("cannot read %s (errno=%d)", GUEST_ELF_PATH, errno);
    printf("HYP_A20OS: guest=%s size=%lu dtb=%lu\n",
           GUEST_ELF_PATH, (unsigned long)img_len, (unsigned long)dtb_len);

    long vm = syscall(SYS_hyp_vm_create, (unsigned long)GUEST_MEM);
    if (vm < 0) {
        free(img);
        free(dtb);
        return fail("vm_create", vm);
    }

    /* The ELF walk loads through the VM, so it happens after the VM exists; a
     * malformed image is reported and the VM dropped. */
    long vcpu;
    uint64_t entry_gpa = 0, image_end = 0;
    int nseg = (int)load_elf(vm, img, img_len, &entry_gpa, &image_end);
    if (nseg < 0) {
        syscall(SYS_hyp_vm_destroy, vm);
        free(img);
        free(dtb);
        return failf("guest ELF rejected (entry=0x%llx)",
                     (unsigned long long)entry_gpa);
    }
    if (image_end > GUEST_BASE + GUEST_MEM) {
        syscall(SYS_hyp_vm_destroy, vm);
        free(img);
        free(dtb);
        return failf("image end 0x%llx past RAM window end 0x%llx",
                     (unsigned long long)image_end,
                     (unsigned long long)(GUEST_BASE + GUEST_MEM));
    }
    if (GUEST_DTB_GPA < image_end) {
        syscall(SYS_hyp_vm_destroy, vm);
        free(img);
        free(dtb);
        return failf("dtb gpa 0x%llx overlaps the image",
                     (unsigned long long)GUEST_DTB_GPA);
    }

    long r = syscall(SYS_hyp_vm_load, vm, (unsigned long)GUEST_DTB_GPA,
                     (unsigned long)dtb, (unsigned long)dtb_len);
    if (r < 0) {
        syscall(SYS_hyp_vm_destroy, vm);
        free(img);
        free(dtb);
        return fail("dtb load", r);
    }

    vcpu = syscall(SYS_hyp_vcpu_create, vm, (unsigned long)entry_gpa);
    if (vcpu < 0) {
        syscall(SYS_hyp_vm_destroy, vm);
        free(img);
        free(dtb);
        return fail("vcpu_create", vcpu);
    }

    /* hartid 0 is not a default, it is the value the guest entry code needs:
     * entry.S branches straight past its SBI HSM lottery handoff on a0 == 0
     * (entry.S:44), and the vcpu slice has exactly one hart to be. */
    r = syscall(SYS_hyp_vcpu_set_boot, vcpu, 0UL, (unsigned long)GUEST_DTB_GPA);
    if (r < 0) {
        syscall(SYS_hyp_vm_destroy, vm);
        free(img);
        free(dtb);
        return fail("vcpu_set_boot", r);
    }

    r = syscall(SYS_hyp_vm_set_marker, vm, (unsigned long)GUEST_MARKER);
    if (r < 0) {
        syscall(SYS_hyp_vm_destroy, vm);
        free(img);
        free(dtb);
        return fail("vm_set_marker", r);
    }

    printf("HYP_A20OS: segments=%d entry_gpa=0x%llx image_end=0x%llx "
           "ram=%lu MiB marker='%s'\n",
           nseg, (unsigned long long)entry_gpa, (unsigned long long)image_end,
           (unsigned long)(GUEST_MEM >> 20), GUEST_MARKER);
    printf("HYP_A20OS: running\n");
    fflush(stdout);

    long exit_reason = syscall(SYS_hyp_vcpu_run, vcpu);

    struct hyp_status st;
    memset(&st, 0, sizeof(st));
    r = syscall(SYS_hyp_vm_status, vm, (unsigned long)&st);

    syscall(SYS_hyp_vm_destroy, vm);
    free(img);
    free(dtb);

    if (exit_reason < 0)
        return fail("vcpu_run", exit_reason);
    if (r < 0)
        return fail("vm_status", r);

    printf("HYP_A20OS: exit=%ld(%s) scause=0x%llx stval=0x%llx htval=0x%llx "
           "marker_seen=%llu console_bytes=%llu\n",
           exit_reason,
           exit_reason == HYP_EXIT_SHUTDOWN ? "shutdown" :
           exit_reason == HYP_EXIT_FAULT   ? "fault" :
           exit_reason == HYP_EXIT_ERROR   ? "error" : "none",
           (unsigned long long)st.scause, (unsigned long long)st.stval,
           (unsigned long long)st.htval,
           (unsigned long long)st.marker_seen,
           (unsigned long long)st.console_bytes);

    /* The marker is matched as a sliding window over guest console bytes, so a
     * hit already means the whole string arrived.  The byte count is checked
     * as well so a report can never read "12 bytes seen, 12-byte marker
     * matched" -- the guest wrote more than the banner and kept going. */
    if (!st.marker_seen)
        return failf("guest never printed '%s' (exit=%ld)", GUEST_MARKER,
                     exit_reason);
    if (st.console_bytes <= (uint64_t)strlen(GUEST_MARKER))
        return failf("guest wrote %llu bytes, marker is %u: hit is degenerate",
                     (unsigned long long)st.console_bytes,
                     (unsigned)strlen(GUEST_MARKER));

    printf("HYP_A20OS: PASS\n");
    return 0;
}
