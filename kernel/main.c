#include "core/stdio.h"
#include "drv/uart.h"
#include "mm/mm.h"
#include "mm/elf.h"
#include "mm/vm.h"
#include "core/trap.h"
#include "proc/proc.h"
#include "sys/syscall.h"
#include "core/timer.h"
#include "core/string.h"
#include "core/consts.h"
#include "abi/current.h"
#include "core/defs.h"
#include "core/panic.h"
#include "core/timekeeping.h"
#include "core/random.h"
#include "fs/vfs.h"
#include "drv/virtio_blk.h"
#include "fs/block_cache.h"
#include "core/klog.h"
#include "net/socket.h"

/* Forward declarations */
void init_kthread(void);

/* ============================================================
 * Block-device mount configuration
 *
 * virtio_blk_init() auto-assigns the next transport slot, so each
 * call probes the next virtio-blk device in sequence.
 * virtio_blk_get_dev(i) then returns the block_dev_t for slot i.
 *
 * CONTEST mode  (competition QEMU):
 *   Device 0 (bus 0) = test EXT4 disk  (provided by judge)
 *   Device 1 (bus 1) = our utilities    (disk.img / disk-la.img)
 *
 * Development mode (run-riscv64 / run-loongarch64):
 *   Device 0 (bus 0) = FAT32 user apps
 *   Device 1 (bus 1) = EXT4 data
 *   Device 2..3      = optional sdcard images (best-effort)
 * ============================================================ */

#ifndef BRINGUP
typedef struct {
    const char *mount_point;
    const char *fs_type;
} mount_entry_t;

#ifdef CONTEST
static const mount_entry_t mount_table[] = {
    { "/test", "ext4"  },   /* Device 0: competition test disk */
    { "/bin",  "fat32" },   /* Device 1: our user-space utilities */
};
#define MOUNT_COUNT  2
#else
static const mount_entry_t mount_table[] = {
    { "/bin", "fat32" },
    { "/mnt",  "ext4"  },
};
#define MOUNT_COUNT  2
#endif /* CONTEST */

/* Optional extra devices (sdcard images) — mounted if present */
static const mount_entry_t extra_mount_table[] = {
    { "/testrv", "ext4" },  /* Device 2 */
    { "/testla", "ext4" },  /* Device 3 */
};
#define EXTRA_MOUNT_COUNT  2

/* Try to initialise, create a block cache, mkdir and mount.
 * Returns 0 on success, <0 on any failure (already logged). */
static int try_mount(int dev_idx, const char *mnt, const char *fstype) {
    block_dev_t *dev = virtio_blk_get_dev(dev_idx);
    if (!dev) {
        printf("[INIT] Device %d: not available\n", dev_idx);
        return -1;
    }
    bcache_t *bc = bcache_create(dev);
    if (!bc) {
        printf("[INIT] Device %d: block-cache creation failed\n", dev_idx);
        return -1;
    }
    int mkret = vfs_mkdir(mnt, 0755);
    if (mkret < 0 && mkret != -EEXIST) {
        printf("[INIT] Device %d: mkdir %s failed (%d)\n", dev_idx, mnt, mkret);
        bcache_destroy(bc);
        return mkret;
    }
    int r = vfs_mount_bc(mnt, fstype, bc);
    if (r == 0) {
        printf("[INIT] Device %d -> %s (%s)\n", dev_idx, mnt, fstype);
    } else {
        printf("[INIT] Device %d: mount %s failed (%d)\n", dev_idx, mnt, r);
        bcache_destroy(bc);
    }
    return r;
}
#endif /* BRINGUP */

void kernel_main(void) {
    printf("\n");
    printf("======================================\n");
    printf("    A20OS Kernel \n");
    printf("======================================\n");
#ifdef CONTEST
    printf("  (contest mode)\n");
#endif
    printf("Initializing system...\n");
    /* Initialize subsystems */
    trap_init();
    printf("[INIT] Trap subsystem initialized\n");

    uart_init();
    printf("[INIT] UART initialized\n");

    timer_init();
    printf("[INIT] Timer initialized\n");

    timekeeping_init();
    printf("[INIT] Timekeeping initialized\n");

    mm_init();
    printf("[INIT] Memory manager initialized\n");

    random_init();
    printf("[INIT] Random subsystem initialized\n");

    vfs_init();
    printf("[INIT] VFS initialized\n");

    net_init();
    printf("[INIT] Network initialized\n");

    /* ---- Mount block devices ---- */
#ifdef BRINGUP
    printf("[INIT] BRINGUP mode: skipping block device probe\n");
#else
    for (int i = 0; i < MOUNT_COUNT; i++) {
        if (virtio_blk_init() != 0) {
            printf("[INIT] Device %d: probe failed, skipping\n", i);
            continue;
        }
        const char *mnt = mount_table[i].mount_point;
        const char *fstype = mount_table[i].fs_type;
        if (try_mount(i, mnt, fstype) != 0) {
            const char *alt = (strcmp(fstype, "ext4") == 0) ? "fat32" : "ext4";
            printf("[INIT] Device %d: retrying as %s\n", i, alt);
            try_mount(i, mnt, alt);
        }
    }

    /* Best-effort: try optional extra sdcard images */
    for (int j = 0; j < EXTRA_MOUNT_COUNT; j++) {
        if (virtio_blk_init() != 0)
            break;  /* no more devices */
        int dev_idx = MOUNT_COUNT + j;
        try_mount(dev_idx,
                  extra_mount_table[j].mount_point,
                  extra_mount_table[j].fs_type);
    }
#endif

    /* Initialize process management */
    proc_init();
    printf("[INIT] Process manager initialized\n");

#ifdef BRINGUP
    printf("[INIT] BRINGUP mode: no userspace init; entering idle loop\n");
    printf("[INIT] System ready\n\n");
    idle_loop();
#else
    /* Create init kthread */
    int ret = proc_alloc(init_kthread);
    if (ret < 0) {
        panic("Failed to create init_kthread");
    }

    printf("[INIT] Starting scheduler...\n");
    printf("[INIT] System ready\n\n");

#ifndef CONTEST
    printf("\033[1;36m");
    printf("    _    ____   ___   ___  ____  \n");
    printf("   / \\  |___ \\ / _ \\ / _ \\/ ___| \n");
    printf("  / _ \\   __) | | | | | | \\___ \\ \n");
    printf(" / ___ \\ / __/| |_| | |_| |___) |\n");
    printf("/_/   \\_\\_____|\\___/ \\___/|____/ \n");
    printf("\033[0m");
    printf("Welcome to AAAAAAAAAAAAAAAAAAAAOS!\n\n");
#endif

    /* Start scheduler — switches to init_kthread. When idle is later
     * restored via context_switch, execution returns here. Enter
     * idle_loop so the scheduler keeps running (wfi + proc_yield). */
    sched();
    idle_loop();
#endif
}

void init_kthread(void) {
    task_t *cur = proc_current();
    kdebug("[INIT] Init process started (pid=%d)\n", cur ? cur->pid : 0);


    kdebug("[INIT] Loading init program...\n");

    /* Open init program from FAT32 filesystem */
    const char *init_path = "/bin/init";
    int fd = vfs_open(init_path, O_RDONLY, 0);
    if (fd < 0) {
        printf("[INIT] Cannot open /bin/init: %d\n", fd);
        kdebug("[INIT] Falling back to ramfs /init...\n");

        init_path = "/init";
        fd = vfs_open(init_path, O_RDONLY, 0);
        if (fd < 0) {
            panic("init: no init program found (tried /bin/init and /init)");
        }
    }

    /* Load ELF program */
    elf_load_info_t info;
    int ret = elf_load(fd, init_path, &info);
    vfs_close(fd);

    if (ret < 0) {
        panic("init: ELF load failed: %d\n", ret);
    }

    kdebug("[INIT] ELF loaded: entry=0x%lx stack=0x%lx\n",
           (unsigned long)info.entry, (unsigned long)info.stack_top);

    /* Set up the initial user stack with argc/argv/envp/auxv so the
     * C runtime (crt1.o / musl) finds a valid stack layout.  Without
     * this, __libc_start_main dereferences garbage and the init
     * process crashes silently before ever reaching main(). */
    char *init_argv[] = { (char *)init_path, NULL };
    uint64_t user_sp = elf_setup_stack(info.stack_top, 1, init_argv, NULL, &info);
    if (user_sp == 0) {
        panic("init: elf_setup_stack failed");
    }

    size_t init_total_vm = 0;
    for (vm_area_t *v = info.mmap; v; v = v->next)
        init_total_vm += (v->end - v->start) / PAGE_SIZE;

    ret = proc_alloc_user_image(info.entry, user_sp, info.pgdir, info.mmap,
                                info.brk, info.stack_top, init_total_vm);
    if (ret < 0) {
        panic("init: proc_alloc_user failed: %d\n", ret);
    }

    kdebug("[INIT] Init process created: pid=%d\n", ret);

    /* Become the init reaper: wait for any children (including the user init
     * process) so they don't become un-reaped zombies. */
    while (1) {
        proc_wait4(-1, NULL, 0);
    }
}
