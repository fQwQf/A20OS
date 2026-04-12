#include "stdio.h"
#include "uart.h"
#include "mm.h"
#include "elf.h"
#include "trap.h"
#include "proc.h"
#include "syscall.h"
#include "fs.h"
#include "timer.h"
#include "plic.h"
#include "string.h"
#include "consts.h"
#include "defs.h"
#include "panic.h"
#include "vfs.h"
#include "virtio_blk.h"
#include "klog.h"

/* Forward declarations */
void init_kthread(void);

void kernel_main(void) {
    printf("\n");
    printf("======================================\n");
    printf("    A20OS Kernel \n");
    printf("======================================\n");
    printf("Initializing system...\n");

    /* Initialize subsystems */
    uart_init();
    printf("[INIT] UART initialized\n");

    timer_init();
    printf("[INIT] Timer initialized\n");

    plic_init();
    printf("[INIT] PLIC initialized\n");

    mm_init();
    printf("[INIT] Memory manager initialized\n");

    vfs_init();
    printf("[INIT] VFS initialized\n");

    /* Initialize virtio-blk for FAT32 filesystem */
    if (virtio_blk_init(0) == 0) {
        printf("[INIT] Virtio-blk initialized\n");
        fs_mkdir("/mnt");
        vfs_mount(NULL, "/mnt", "fat32", 0);
    } else {
        printf("[INIT] Warning: Virtio-blk initialization failed\n");
    }

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

    printf("\033[1;36m"); // 设置青色高亮
    printf("    _    ____   ___   ___  ____  \n");
    printf("   / \\  |___ \\ / _ \\ / _ \\/ ___| \n");
    printf("  / _ \\   __) | | | | | | \\___ \\ \n");
    printf(" / ___ \\ / __/| |_| | |_| |___) |\n");
    printf("/_/   \\_\\_____|\\___/ \\___/|____/ \n");
    printf("\033[0m"); // 重置颜色
    printf("Welcome to AAAAAAAAAAAAAAAAAAAAOS!\n\n");

    /* Start scheduler — switches to init_kthread. When idle is later
     * restored via context_switch, execution returns here. Enter
     * idle_loop so the scheduler keeps running (wfi + proc_yield). */
    sched();
    idle_loop();
}

void init_kthread(void) {
    task_t *cur = proc_current();
    kdebug("[INIT] Init process started (pid=%d)\n", cur ? cur->pid : 0);

    for (int i = 0; i < 1000000; i++) {
        __asm__ volatile("nop");
    }

    kdebug("[INIT] Loading init program...\n");

    /* Open init program from FAT32 filesystem */
    int fd = vfs_open("/mnt/init", O_RDONLY, 0);
    if (fd < 0) {
        printf("[INIT] Cannot open /mnt/init: %d\n", fd);
        kdebug("[INIT] Falling back to ramfs /init...\n");

        fd = vfs_open("/init", O_RDONLY, 0);
        if (fd < 0) {
            panic("init: no init program found (tried /mnt/init and /init)");
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
