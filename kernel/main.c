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
 * Block-device mount — unified strategy
 *
 * Probe all virtio-blk devices.  Auto-detect by filesystem type:
 *   fat32 → /bin   (our utilities: init, mksh, cmds, …)
 *   ext4  → /test  (judge sdcard or local sdcard image)
 *
 * Works regardless of device ordering:
 *   Contest QEMU:  dev0=ext4(sdcard) dev1=fat32(disk.img)
 *   Dev QEMU:      dev0=fat32(disk.img) dev1=ext4(sdcard)
 * ============================================================ */

#ifndef BRINGUP
static int try_mount(int dev_idx, const char *mnt, const char *fstype) {
    block_dev_t *dev = virtio_blk_get_dev(dev_idx);
    if (!dev) return -1;
    bcache_t *bc = bcache_create(dev);
    if (!bc) return -1;
    int mkret = vfs_mkdir(mnt, 0755);
    if (mkret < 0 && mkret != -EEXIST) {
        bcache_destroy(bc);
        return mkret;
    }
    int r = vfs_mount_bc(mnt, fstype, bc);
    if (r == 0) {
        printf("[INIT] Device %d -> %s (%s)\n", dev_idx, mnt, fstype);
    } else {
        bcache_destroy(bc);
    }
    return r;
}

static void mount_block_devices(void) {
    int bin_ok = 0, test_ok = 0;

    for (int i = 0; i < 8; i++) {
        if (virtio_blk_init() != 0)
            break;

        if (!bin_ok && try_mount(i, "/bin", "fat32") == 0) {
            bin_ok = 1;
            continue;
        }
        if (!test_ok && try_mount(i, "/test", "ext4") == 0) {
            test_ok = 1;
            continue;
        }
    }

    if (!bin_ok)  printf("[INIT] WARNING: no FAT32 device for /bin\n");
    if (!test_ok) printf("[INIT] no ext4 device for /test (ok without sdcard)\n");
}
#endif /* BRINGUP */

void kernel_main(void) {
    printf("\n");
    printf("======================================\n");
    printf("    A20OS Kernel \n");
    printf("======================================\n");
    printf("Initializing system...\n");

    trap_init();
    printf("[INIT] Trap initialized\n");
    uart_init();
    printf("[INIT] UART initialized\n");
    timer_init();
    printf("[INIT] Timer initialized\n");
    timekeeping_init();
    printf("[INIT] Timekeeping initialized\n");
    mm_init();
    printf("[INIT] Memory initialized\n");
    random_init();
    printf("[INIT] Random initialized\n");
    vfs_init();
    printf("[INIT] VFS initialized\n");
    net_init();
    printf("[INIT] Network initialized\n");

#ifdef BRINGUP
    printf("[INIT] BRINGUP mode: no block devices\n");
#else
    mount_block_devices();
#endif

    proc_init();
    printf("[INIT] Process manager initialized\n");

#ifdef BRINGUP
    printf("[INIT] System ready (bringup, no userspace)\n\n");
    idle_loop();
#else
    int ret = proc_alloc(init_kthread);
    if (ret < 0)
        panic("Failed to create init_kthread");

    printf("[INIT] System ready\n\n");
    printf("\033[1;36m");
    printf("    _    ____   ___   ___  ____  \n");
    printf("   / \\  |___ \\ / _ \\ / _ \\/ ___| \n");
    printf("  / _ \\   __) | | | | | | \\___ \\ \n");
    printf(" / ___ \\ / __/| |_| | |_| |___) |\n");
    printf("/_/   \\_\\_____|\\___/ \\___/|____/ \n");
    printf("\033[0m");
    printf("Welcome to A20OS!\n\n");

    sched();
    idle_loop();
#endif
}

void init_kthread(void) {
    task_t *cur = proc_current();
    kdebug("[INIT] Init process started (pid=%d)\n", cur ? cur->pid : 0);

    kdebug("[INIT] Loading init program...\n");

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
        int ret = proc_wait4(-1, NULL, 0);
        if (ret == -ECHILD)
            proc_yield();
    }
}
