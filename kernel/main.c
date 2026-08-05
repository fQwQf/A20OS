#include "core/stdio.h"
#include "core/bootargs.h"
#include "drivers/char/uart.h"
#include "mm/mm.h"
#include "mm/elf.h"
#include "mm/vm.h"
#include "core/trap.h"
#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "sys/syscall.h"
#include "core/timer.h"
#include "core/smp.h"
#include "core/string.h"
#include "core/consts.h"
#include "core/defs.h"
#include "core/panic.h"
#include "core/timekeeping.h"
#include "core/random.h"
#include "fs/vfs.h"
#include "fs/mount_setup.h"
#include "drivers/block/virtio_blk.h"
#include "drivers/block/virtio_scsi.h"
#ifdef CONFIG_AHCI
#include "drivers/block/ahci.h"
#endif
#include "drivers/gpu/virtio_gpu.h"
#include "drivers/input/virtio_input.h"
#ifdef CONFIG_PS2_INPUT
#include "drivers/input/ps2.h"
#endif
#include "fs/block_cache.h"
#include "core/klog.h"
#include "proc/signal.h"
#include "drivers/block/loop.h"
#include "net/socket.h"
#include "drivers/core/driver_core.h"
#include "drivers/usb/usb.h"
#include "drivers/usb/usb_storage.h"
#ifdef CONFIG_SWAP
#include "mm/swap.h"
#endif
#ifdef CONFIG_DRIVER_LIFECYCLE_TEST
#include "drivers/core/driver_lifecycle_test.h"
#endif

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


#ifdef BRINGUP
static void bringup_smoke_test(void) {
    int ok = 1;
    void *p = kmalloc(64);
    if (!p)
        ok = 0;
    kfree(p);
    if (ok)
        printf("part ok\n");
    printf("System is going down for power-off NOW\n");
    firmware_shutdown();
}
#endif

void kernel_main(void) {
    printf("\n");
    printf("======================================\n");
    printf("    A20OS Kernel \n");
    printf("======================================\n");
    printf("Initializing system...\n");

    trap_init();
    printf("[INIT] Trap initialized\n");
    if (current_board && current_board->early_init) {
        current_board->early_init();
        printf("[INIT] Board early init done\n");
    }
    uart_init();
    printf("[INIT] UART initialized\n");
    timer_init();
    printf("[INIT] Timer initialized\n");
    timekeeping_init();
    printf("[INIT] Timekeeping initialized\n");
    mm_init();
    printf("[INIT] Memory initialized\n");
    timekeeping_vdso_init();
#ifdef CONFIG_SWAP
    swap_init();
#endif
    random_init();
    printf("[INIT] Random initialized\n");
    bootargs_init();
    printf("[INIT] Boot arguments parsed\n");
    driver_core_init();
    printf("[INIT] Driver core initialized\n");
    usb_core_init();
    printf("[INIT] USB core initialized\n");
    if (current_board && current_board->enumerate_devices) {
        current_board->enumerate_devices();
        printf("[INIT] Board devices enumerated (%s)\n",
               current_board->name ? current_board->name : "unknown");
    }
    driver_probe_all();
    printf("[INIT] Drivers probed\n");
    usb_core_scan();
    printf("[INIT] USB devices scanned\n");
#ifdef CONFIG_PS2_INPUT
    if (ps2_input_init() != 0)
        printf("[INIT] PS/2 input controller unavailable\n");
    else
        printf("[INIT] PS/2 input initialized\n");
#endif
#ifdef CONFIG_DRIVER_LIFECYCLE_TEST
    driver_lifecycle_test_run();
#endif
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
    smp_init();
    smp_boot_secondaries();
    arch_unmap_boot_identity();
    loop_init();

#ifdef BRINGUP
    printf("[INIT] System ready (bringup, no userspace)\n\n");
    bringup_smoke_test();
#else
#ifdef CONFIG_COOPERATIVE_BOOT
    /* VBox has no usable preemption timer yet.  Bootstrap PID 1 directly from
     * the idle context instead of requiring an otherwise unnecessary first
     * kernel-thread switch before userspace can exist. */
    printf("[INIT] bootstrapping userspace directly on VirtualBox\n");
#else
    int ret = proc_alloc(init_kthread);
    if (ret < 0)
        panic("Failed to create init_kthread");
#endif

    printf("[INIT] System ready\n\n");
    printf("\033[1;36m");
    printf("%s\n", "                    :%%%%%%%.                                 ");
    printf("%s\n", "                    %%%%%%%*                                  ");
    printf("%s\n", "                   -%%%%%%%                                   ");
    printf("%s\n", "                   %%%%%%%-                                   ");
    printf("%s\n", "                  -%%%%%%%                                    ");
    printf("%s\n", "                  %%%%%%%=                                    ");
    printf("%s\n", "                 *%%%%%%%                                     ");
    printf("%s\n", "                .%%%%%%%:=+                                   ");
    printf("%s\n", "                @%%%%%%% %%                                   ");
    printf("%s\n", "                %%%%%%% +%%+                                  ");
    printf("%s\n", "               #%%%%%%+ %%%%                                  ");
    printf("%s\n", "              :%%%%%%% %%%%%-                                 ");
    printf("%s\n", "              %%%%%%%# %%%%%%    ........  .........          ");
    printf("%s\n", "             =%%%%%%% -%%%%%%-   ######### #########=         ");
    printf("%s\n", "             %%%%%%%=  %%%%%%#   ++*+*+*## #*++++++#=         ");
    printf("%s\n", "            *%%%%%%%   :%%%%%%:         ## #+      #=         ");
    printf("%s\n", "            %%%%%%%.    %%%%%%%         ## #+      #=         ");
    printf("%s\n", "           +%%%%%%%     =%%%%%%         *# #+      #=         ");
    printf("%s\n", "           %%%%%%%:      %%%%%%*  ######## #+      #=         ");
    printf("%s\n", "          %%%%%%%%       %%%%%%% :######## #+      #=         ");
    printf("%s\n", "         :%%%%%%%.        %%%%%%::#=       #+      #=         ");
    printf("%s\n", "         %%%%%%%* %%%%%%%%%%%%%%::#=       #+      #=         ");
    printf("%s\n", "        :%%%%%%% -%%%%%%%%%%%%%%::#=       #+      #=         ");
    printf("%s\n", "        %%%%%%%: %%%%%%%%%%%%%%%::######## #########=         ");
    printf("%s\n", "       :======= .===============. -------- ---------:    ");
    printf("\033[0m");
    printf("Welcome to A20OS!\n\n");
    printf("[INIT] entering scheduler...\n");

#ifdef CONFIG_COOPERATIVE_BOOT
    init_kthread();
#else
    sched();
    idle_loop();
#endif
#endif
}

void init_kthread(void) {
    task_t *cur = proc_current();
    printf("[INIT] init_kthread started (pid=%d)\n", cur ? cur->pid : 0);

    const char *init_path = "/bin/init";
    printf("[INIT] opening %s...\n", init_path);
    int fd = vfs_open(init_path, O_RDONLY, 0);
    if (fd < 0) {
        printf("[INIT] Cannot open /bin/init: %d\n", fd);

        init_path = "/init";
        fd = vfs_open(init_path, O_RDONLY, 0);
        if (fd < 0) {
            panic("init: no init program found (tried /bin/init and /init)");
        }
    }

    /* Print enough information to distinguish a stale converted VDI from the
     * image that was just built.  The hash is diagnostic (FNV-1a), not a
     * security primitive; it deliberately avoids pulling crypto into PID 1
     * bootstrap. */
    kstat_t init_stat;
    union {
        Elf32_Ehdr e32;
        Elf64_Ehdr e64;
    } init_ehdr = {0};
    uint64_t init_hash = 14695981039346656037ULL;
    uint64_t init_size = 0;
    if (vfs_fstat(fd, &init_stat) == 0)
        init_size = init_stat.st_size;
    int ehdr_len = vfs_pread(fd, (char *)&init_ehdr, sizeof(init_ehdr), 0);
    char hash_buf[1024];
    for (uint64_t off = 0; off < init_size; off += sizeof(hash_buf)) {
        size_t want = (size_t)(init_size - off);
        if (want > sizeof(hash_buf))
            want = sizeof(hash_buf);
        int got = vfs_pread(fd, hash_buf, want, off);
        if (got <= 0)
            break;
        for (int i = 0; i < got; i++) {
            init_hash ^= (uint8_t)hash_buf[i];
            init_hash *= 1099511628211ULL;
        }
        if ((size_t)got != want)
            break;
    }
    const uint8_t *ident = init_ehdr.e64.e_ident;
    int valid_ident = ehdr_len >= 16 && ident[0] == 0x7f && ident[1] == 'E' &&
                      ident[2] == 'L' && ident[3] == 'F';
    uint16_t image_machine = 0;
    uint64_t image_entry = 0;
    if (valid_ident && ident[4] == ELFCLASS64 &&
        ehdr_len >= (int)sizeof(init_ehdr.e64)) {
        image_machine = init_ehdr.e64.e_machine;
        image_entry = init_ehdr.e64.e_entry;
    } else if (valid_ident && ident[4] == ELFCLASS32 &&
               ehdr_len >= (int)sizeof(init_ehdr.e32)) {
        image_machine = init_ehdr.e32.e_machine;
        image_entry = init_ehdr.e32.e_entry;
    } else {
        valid_ident = 0;
    }
    if (valid_ident) {
        printf("[INIT] image: size=%lu fnv1a64=%016lx class=%u machine=%u file-entry=0x%lx\n",
               (unsigned long)init_size, (unsigned long)init_hash,
               (unsigned)ident[4], (unsigned)image_machine,
               (unsigned long)image_entry);
    } else {
        printf("[INIT] image: size=%lu fnv1a64=%016lx (invalid ELF header)\n",
               (unsigned long)init_size, (unsigned long)init_hash);
    }

    printf("[INIT] loading ELF...\n");
    elf_load_info_t info;
    int ret = elf_load(fd, init_path, &info);
    vfs_close(fd);

    if (ret < 0) {
        panic("init: ELF load failed: %d\n", ret);
    }

    printf("[INIT] ELF loaded: entry=0x%lx stack=0x%lx\n",
           (unsigned long)info.entry, (unsigned long)info.stack_top);

#ifndef CONFIG_NOMMU
    mm_leaf_info_t entry_leaf;
    uint32_t entry_insn = 0;
    int entry_present = mm_query_leaf(info.pgdir, info.entry, &entry_leaf);
    if (entry_present) {
        if (!(entry_leaf.flags & PTE_U) ||
#if PTE_X != 0
            !(entry_leaf.flags & PTE_X) ||
#endif
            !mm_fetch_user_insn32(info.pgdir, info.entry, &entry_insn))
            panic("init: entry is not a readable user executable mapping");
        printf("[INIT] entry map: pa=0x%lx flags=0x%lx insn=0x%08x\n",
               (unsigned long)entry_leaf.pa, (unsigned long)entry_leaf.flags,
               entry_insn);
    } else {
        vm_area_t *entry_vma = info.mmap;
        while (entry_vma &&
               !(info.entry >= entry_vma->start && info.entry < entry_vma->end))
            entry_vma = entry_vma->next;
        if (!entry_vma ||
#if PTE_X != 0
            !(entry_vma->pte_flags & PTE_X) ||
#endif
            !(entry_vma->pte_flags & PTE_U))
            panic("init: entry is not an executable demand mapping");
        printf("[INIT] entry map: demand-paged flags=0x%lx\n",
               (unsigned long)entry_vma->pte_flags);
    }
#endif

    /* Set up the initial user stack with argc/argv/envp/auxv so the
     * C runtime (crt1.o / musl) finds a valid stack layout.  Without
     * this, __libc_start_main dereferences garbage and the init
     * process crashes silently before ever reaching main().
     */
    char *init_argv[] = { (char *)init_path, NULL };
    /* pid 2 keeps the syscall time path (no vDSO); everything it execs
     * gets the vDSO through the regular exec path (proc/exec.c). */
    uint64_t user_sp = elf_setup_stack(info.stack_top, 1, init_argv, NULL, &info, 0);
    if (user_sp == 0) {
        panic("init: elf_setup_stack failed");
    }
    printf("[INIT] user_sp=0x%lx\n", (unsigned long)user_sp);

    size_t init_total_vm = 0;
    for (vm_area_t *v = info.mmap; v; v = v->next)
        init_total_vm += (v->end - v->start) / PAGE_SIZE;

    ret = proc_alloc_user_image(info.entry, user_sp, info.pgdir, info.mmap,
                                info.brk, info.stack_top, init_total_vm,
                                info.tls_tp
#ifdef CONFIG_NOMMU
                                , info.nommu_allocs, info.nommu_alloc_sizes,
                                info.nommu_alloc_types, info.num_nommu_allocs
#endif
                                , 0);
    if (ret < 0) {
        panic("init: proc_alloc_user failed: %d\n", ret);
    }

#ifdef CONFIG_NOMMU
    arch_flush_icache_range((const void *)info.load_addr, info.load_size);
#else
    /* map_segment() cleaned executable pages through their direct-map alias;
     * discard any stale instruction lines before this image first runs. */
    arch_fence_i();
#endif

    printf("[INIT] user init created: pid=%d entry=0x%lx sp=0x%lx\n",
           ret, (unsigned long)info.entry, (unsigned long)user_sp);

#ifdef CONFIG_COOPERATIVE_BOOT
    /* PID 0 remains the reaper and scheduler host. */
    idle_loop();
#endif

    /* Become the init reaper: wait for any children (including the user init
     * process) so they don't become un-reaped zombies. */
    while (1) {
        int ret = proc_wait4(-1, NULL, 0);
        if (ret == -ECHILD)
            proc_yield();
    }
}
