#include "stdio.h"
#include "uart.h"
#include "mm.h"
#include "elf.h"
#include "trap.h"
#include "proc.h"
#include "syscall.h"
#include "fs.h"
#include "timer.h"
#include "plat_irq.h"
#include "string.h"
#include "consts.h"
#include "defs.h"
#include "panic.h"
#include "vfs.h"
#include "virtio_blk.h"
#include "block_cache.h"
#include "klog.h"
#include "arch_ops.h"

/* Forward declarations */
void init_kthread(void);

/* ============================================================
 * Block-device mount configuration
 *
 * virtio_blk_init(0) auto-assigns the next MMIO slot, so each
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
    { "/bin", "fat32" },    /* Device 0: user apps (FAT32) */
    { "/mnt", "ext4"  },    /* Device 1: ext4 data */
};
#define MOUNT_COUNT  2

/* Optional extra devices (sdcard images) — mounted if present */
static const mount_entry_t extra_mount_table[] = {
    { "/testrv", "ext4" },  /* Device 2 */
    { "/testla", "ext4" },  /* Device 3 */
};
#define EXTRA_MOUNT_COUNT  2
#endif /* CONTEST */

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
    fs_mkdir(mnt);
    int r = vfs_mount_bc(mnt, fstype, bc);
    if (r == 0) {
        printf("[INIT] Device %d -> %s (%s)\n", dev_idx, mnt, fstype);
    } else {
        printf("[INIT] Device %d: mount %s failed (%d)\n", dev_idx, mnt, r);
    }
    return r;
}

void kernel_main(void) {
    uart_init();

    printf("\n");
    printf("======================================\n");
    printf("    A20OS Kernel \n");
    printf("======================================\n");
#ifdef CONTEST
    printf("  (contest mode)\n");
#endif
    printf("Initializing system...\n");
    printf("[INIT] UART initialized\n");

    timer_init();
    printf("[INIT] Timer initialized\n");

    plat_irq_init();
    printf("[INIT] IRQ controller initialized\n");

    mm_init();
    printf("[INIT] Memory manager initialized\n");

    vfs_init();
    printf("[INIT] VFS initialized\n");

    /* ---- Mount block devices ---- */
    for (int i = 0; i < MOUNT_COUNT; i++) {
        if (virtio_blk_init(0) != 0) {
            printf("[INIT] Device %d: probe failed, skipping\n", i);
            continue;
        }
        try_mount(i, mount_table[i].mount_point, mount_table[i].fs_type);
    }

#ifndef CONTEST
    /* Best-effort: try optional extra sdcard images */
    for (int j = 0; j < EXTRA_MOUNT_COUNT; j++) {
        if (virtio_blk_init(0) != 0)
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

    sched();
    idle_loop();
}

void init_kthread(void) {
    task_t *cur = proc_current();
    kdebug("[INIT] Init process started (pid=%d)\n", cur ? cur->pid : 0);

    for (int i = 0; i < 1000000; i++) {
        arch_cpu_relax();
    }

    kdebug("[INIT] Loading init program...\n");

    /* Open init program from FAT32 filesystem */
    int fd = vfs_open("/bin/init", O_RDONLY, 0);
    if (fd < 0) {
        printf("[INIT] Cannot open /bin/init: %d\n", fd);
        kdebug("[INIT] Falling back to ramfs /init...\n");

        fd = vfs_open("/init", O_RDONLY, 0);
        if (fd < 0) {
            panic("init: no init program found (tried /bin/init and /init)");
        }
    }

    /* Load ELF program */
    elf_load_info_t info;
    int ret = elf_load(fd, &info);
    vfs_close(fd);

    if (ret < 0) {
        panic("init: ELF load failed: %d\n", ret);
    }

    kdebug("[INIT] ELF loaded: entry=0x%lx stack=0x%lx\n",
           (unsigned long)info.entry, (unsigned long)info.stack_top);

    ret = proc_alloc_user(info.entry, info.stack_top, info.pgdir);
    if (ret < 0) {
        panic("init: proc_alloc_user failed: %d\n", ret);
    }

    kdebug("[INIT] Init process created: pid=%d\n", ret);

    proc_exit(0);
}
