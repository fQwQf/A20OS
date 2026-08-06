#include "core/stdio.h"
#include "core/bootargs.h"
#include "drivers/char/uart.h"
void riscv_iommu_early_probe(void);
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
#include "fs/block_cache.h"
#include "core/klog.h"
#include "proc/signal.h"
#include "drivers/block/loop.h"
#include "net/socket.h"
#include "drivers/core/driver_core.h"
#include "drvmod/drvmod.h"
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
#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
    extern void a20_registry_init(void);
#endif
    extern void kep_syscall_filter_init(void);
    printf("\n");
    printf("======================================\n");
    printf("    A20OS Kernel \n");
    printf("======================================\n");
    printf("Initializing system...\n");

#if defined(CONFIG_UBSAN) && CONFIG_UBSAN
    extern void ubsan_selftest(void);
    ubsan_selftest();
#endif

    trap_init();
    printf("[INIT] Trap initialized\n");
    kep_syscall_filter_init();
    printf("[INIT] Kernel extension points initialized\n");
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
#ifdef CONFIG_BOARD_QEMU_VIRT_RISCV64
    riscv_iommu_early_probe();
#endif
    driver_probe_all();
    printf("[INIT] Drivers probed\n");
#ifdef CONFIG_BOARD_QEMU_VIRT_RISCV64
    /* Dual-placement driver skeleton (docs/hybrid-kernel/04-dual-placement.md):
     * the kernel placements of the shared goldfish RTC and virtio-input
     * drivers are drvmod modules (/lib/drivers/rtc.drv, vinput-probe.drv)
     * loaded and bound in init_kthread below; the built-in probes were
     * removed by the drvmod migration. */
#endif
    usb_core_scan();
    printf("[INIT] USB devices scanned\n");
    /* PS/2 controller (x86_64) is owned by the ps2.drv drvmod module;
     * the built-in init was removed by the drvmod migration. */
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
#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
    a20_registry_init();
    printf("[INIT] Service registry initialized\n");
#endif
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

    /* Driver modules (drvmod): register the hardware devices the modules
     * may bind to, then scan the DriverStore (/bin/lib/drivers, i.e. the
     * FAT32 /lib/drivers) for *.drv modules, load them and run the
     * automatic binding pass (kernel/drvmod/).  Modules staged by
     * `drvctl install` are therefore activated on the next boot. */
    {
#if defined(CONFIG_X86_64)
        static drv_device_t g_tpm_dev = { 0 };
        strncpy(g_tpm_dev.name, "tpm", sizeof(g_tpm_dev.name) - 1);
        g_tpm_dev.bus = 0;                    /* fixed/system */
        g_tpm_dev.vendor = 0x54504D00UL;      /* "TPM\0" */
        g_tpm_dev.device = 0;
        g_tpm_dev.irq = -1;
        drv_device_register(&g_tpm_dev);

        static drv_device_t g_ps2_dev = { 0 };
        strncpy(g_ps2_dev.name, "ps2", sizeof(g_ps2_dev.name) - 1);
        g_ps2_dev.bus = 0;                    /* fixed/system */
        g_ps2_dev.vendor = 0x50533200UL;      /* "PS2" */
        g_ps2_dev.device = 0;
        g_ps2_dev.irq = IRQ_VECTOR_KEYBOARD;
        drv_device_register(&g_ps2_dev);
#endif

        static drv_device_t g_grtc_dev = { 0 };
        strncpy(g_grtc_dev.name, "goldfish-rtc", sizeof(g_grtc_dev.name) - 1);
        g_grtc_dev.bus = 3;                       /* mmio */
        g_grtc_dev.vendor = 0x101000UL;
        g_grtc_dev.device = 0;
        g_grtc_dev.mmio_phys = 0x101000UL;
        g_grtc_dev.mmio_size = 0x100UL;
        g_grtc_dev.irq = -1;
        drv_device_register(&g_grtc_dev);

        /* virtio-input slot 5 (dual-placement kernel probe module).  The
         * virtio-mmio slot base differs per board: aarch64 slots are 0x200
         * apart (0x0A000000 base), riscv64 slots are 0x1000 apart
         * (0x10001000 base).  LoongArch64 has no virtio-mmio bus (devices
         * arrive over PCI) so no slot device is registered there. */
#if defined(CONFIG_BOARD_QEMU_VIRT_AARCH64)
        static drv_device_t g_vinput_dev = { 0 };
        strncpy(g_vinput_dev.name, "virtio-input-slot5",
                sizeof(g_vinput_dev.name) - 1);
        g_vinput_dev.bus = 3;
        g_vinput_dev.vendor = 0x0A000A00UL;
        g_vinput_dev.device = 0;
        g_vinput_dev.irq = -1;
        drv_device_register(&g_vinput_dev);
#elif defined(CONFIG_BOARD_QEMU_VIRT_RISCV64)
        static drv_device_t g_vinput_dev = { 0 };
        strncpy(g_vinput_dev.name, "virtio-input-slot5",
                sizeof(g_vinput_dev.name) - 1);
        g_vinput_dev.bus = 3;
        g_vinput_dev.vendor = 0x10006000UL;
        g_vinput_dev.device = 0;
        g_vinput_dev.irq = -1;
        drv_device_register(&g_vinput_dev);
#endif

        static const char store[] = "/bin/lib/drivers";
        int dfd = vfs_open(store, O_RDONLY, 0);
        if (dfd < 0) {
            printf("[INIT] driver store %s not found: %d\n", store, dfd);
        } else {
            char dents[512];
            for (;;) {
                int n = vfs_getdents64(dfd, dents, sizeof(dents));
                if (n <= 0)
                    break;
                for (int off = 0; off + (int)offsetof(vfs_dirent64_t,
                                                      d_name) < n; ) {
                    vfs_dirent64_t *de = (vfs_dirent64_t *)(dents + off);
                    if (de->d_reclen < offsetof(vfs_dirent64_t, d_name) ||
                        off + de->d_reclen > n)
                        break;
                    const char *nm = de->d_name;
                    size_t nlen = strlen(nm);
                    if (nlen > 4 && strcmp(nm + nlen - 4, ".drv") == 0) {
                        char path[128];
                        snprintf(path, sizeof(path), "%s/%s", store, nm);
                        int mfd = vfs_open(path, O_RDONLY, 0);
                        if (mfd < 0) {
                            printf("[INIT] driver module %s not found\n",
                                   path);
                        } else {
                            int mid = drvmod_load(mfd, nm);
                            vfs_close(mfd);
                            if (mid < 0)
                                printf("[INIT] driver module %s load "
                                       "failed: %d\n", nm, mid);
                        }
                    }
                    off += de->d_reclen;
                }
            }
            vfs_close(dfd);
        }
        drvmod_init_all();
        drvmod_bind_all();
    }

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
