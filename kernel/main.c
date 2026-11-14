#include "core/stdio.h"
#include "boot/context.h"
#include "firmware/device.h"
#ifdef CONFIG_ACPI
#include "firmware/acpi.h"
#include "firmware/ivrs.h"
#include "drivers/core/amd_iommu.h"
extern void acpi_os_start_worker(void);
#endif
#include "core/bootargs.h"
#include "hyp/hyp.h"
#include "drivers/char/uart.h"
#include "drivers/char/fbcon.h"
#include "mm/mm.h"
#include "mm/oom.h"
#include "mm/elf.h"
#include "mm/vm.h"
#include "mm/pt.h"
#include "core/trap.h"
#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "proc/xlator.h"
#include "sys/syscall.h"
#include "core/timer.h"
#include "core/smp.h"
#include "core/smp_test.h"
#include "core/string.h"
#include "core/consts.h"
#include "core/defs.h"
#include "core/panic.h"
#include "core/timekeeping.h"
#include "core/random.h"
#include "core/stack_protector.h"
#include "fs/vfs.h"
#include "fs/mount_setup.h"
#include "drivers/block/virtio_blk.h"
#include "drivers/gpu/virtio_gpu.h"
#include "drivers/input/virtio_input.h"
#include "fs/block_cache.h"
#include "core/klog.h"
#include "proc/signal.h"
#include "drivers/block/loop.h"
#include "net/socket.h"
#include "net/stack.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/memory.h"
#include "drivers/core/runtime_test.h"
#include "drvmod/drvmod.h"
#include "drivers/usb/usb.h"
#include "drivers/usb/usb_storage.h"
#ifdef CONFIG_SWAP
#include "mm/swap.h"
#endif
#ifdef CONFIG_DRIVER_LIFECYCLE_TEST
#include "drivers/core/driver_lifecycle_test.h"
#endif
#ifdef CONFIG_PLATFORM_IRQ_TEST
#include "drivers/core/platform_irq_test.h"
#endif
#ifdef CONFIG_VMO_PAGE_TEST
extern void vmo_page_selftest_run(void);
#endif
#ifdef CONFIG_RUST_RUNTIME_PANIC_TEST
extern void a20_rust_runtime_panic_test(void);
#endif

/* Forward declarations */
void init_kthread(void);

/* Architecture-owned bootarg diagnostics are a no-op unless an architecture
 * provides a real implementation. */
__attribute__((weak)) void arch_run_platform_selftests(void) {
}

__attribute__((weak)) void arch_run_bootarg_selftests(void) {
}

/*
 * Boot self-tests that belong to a driver but need no device.  Each driver
 * provides its own hook; a build without that driver keeps the weak default,
 * which reports -ENOSYS and is skipped rather than printed as a failure.
 * They must not touch hardware: the point is to pin down the parts of the
 * driver whose constants and bit arithmetic can be checked without a bus.
 */
__attribute__((weak)) int a20_boot_selftest_i2c_hid(void) { return -ENOSYS; }
__attribute__((weak)) int a20_boot_selftest_r8169(void) { return -ENOSYS; }
__attribute__((weak)) int a20_boot_selftest_i2c_dw(void) { return -ENOSYS; }
__attribute__((weak)) int a20_boot_selftest_dma_window(void) { return -ENOSYS; }
__attribute__((weak)) void a20_i2c_hid_start_worker(void) {}

static void a20_run_one_selftest(const char *name, int (*fn)(void))
{
    int rc = fn();
    if (rc == -ENOSYS)
        return;
    if (rc == 0)
        kinfo("[SELFTEST] %s: PASS\n", name);
    else
        kerr("[SELFTEST] %s: FAIL\n", name);
}

__attribute__((weak)) void a20_driver_boot_selftests(void) {
    a20_run_one_selftest("i2c-hid", a20_boot_selftest_i2c_hid);
    a20_run_one_selftest("r8169", a20_boot_selftest_r8169);
    a20_run_one_selftest("i2c-dw", a20_boot_selftest_i2c_dw);
#ifdef CONFIG_WIFI_RTW89
    a20_run_one_selftest("dma-window", a20_boot_selftest_dma_window);
#endif
}

/* User thread-pointer restore after a context switch defaults to nothing:
 * most architectures keep the user TLS in state that __switch already
 * carries.  x86_64 overrides this in trap/irqchip.c. */
__attribute__((weak)) void arch_switch_tls_restore(void) {
}

/* exec(2) publishes the new image's TLS; weak no-op unless the architecture
 * keeps the user thread pointer in an MSR that outlives the exec (x86_64). */
__attribute__((weak)) void arch_publish_user_tls(uint64_t base) {
    (void)base;
}

/* ============================================================
 * Block-device mount — unified strategy
 *
 * Probe all virtio-blk devices.  Auto-detect by filesystem type:
 *   fat32 → /bin   (our utilities: init, mksh, cmds, …)
 *   ext4  → /extra  (local sdcard image)
 *
 * Works regardless of device ordering:
 *   Alt QEMU:  dev0=ext4(sdcard) dev1=fat32(disk.img)
 *   Dev QEMU:      dev0=fat32(disk.img) dev1=ext4(sdcard)
 * ============================================================ */

#if defined(BRINGUP) && !defined(CONFIG_RAMFS_USER)
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

static void boot_advance(enum boot_phase expected, enum boot_phase next) {
    if (boot_context_advance(expected, next) < 0)
        panic("boot: invalid initialization phase transition");
    /* Flush at every phase boundary.  This is process context and happens
     * before the step that could hang, so the persistent log always names the
     * last completed phase even if the next one never returns. */
    klog_disk_sync();
}

#if CONFIG_KLOG_ENABLED
/* One-shot diagnostic report, deliberately late: the shell has started, the
 * USB stack has had time to fail its first command, and the user is looking
 * at a screen they cannot scroll.  It is bounded and self-contained, so the
 * last lines (the fatal controller state) stay visible without a keyboard. */
static void boot_report_thread(void) {
    proc_set_name(proc_current(), "bootreport");
    /* Wait long enough for the shell to start, but break out as soon as the
     * xHCI fatal block lands so the user does not have to time a screenshot.
     * Ten seconds is the ceiling when no fatal appears. */
    for (unsigned i = 0; i < 20; i++) {
        proc_sleep_until(timer_get_ticks() + MS_TO_TICKS(500));
        if (klog_has_fatal()) {
            proc_sleep_until(timer_get_ticks() + MS_TO_TICKS(500));
            break;
        }
    }
    klog_boot_report();
    for (;;)
        proc_sleep_until(timer_get_ticks() + MS_TO_TICKS(3600000));
}
#endif

void kernel_main(void) {
    arch_boot_context_init();
    /* Allocate this boot's random identity before anything can log.  The
     * persistent writer names its file after it, and the loader records the
     * same boot sequence in the ESP breadcrumb. */
    klog_init();
    /* Publish the firmware scanout as a text console before the first banner
     * line.  A machine with no COM1 has no other place for that output. */
    fbcon_init();
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
#ifdef CONFIG_RUST_RUNTIME_PANIC_TEST
    a20_rust_runtime_panic_test();
#endif
    timer_init();
    printf("[INIT] Timer initialized\n");
    timekeeping_init();
    printf("[INIT] Timekeeping initialized\n");
    boot_advance(BOOT_PHASE_HANDOFF, BOOT_PHASE_EARLY);
    mm_init();
    boot_advance(BOOT_PHASE_EARLY, BOOT_PHASE_MEMORY);
    printf("[INIT] Memory initialized\n");
    /* A GOP scanout above 4 GiB is only mappable now that frames exist.  This
     * is a no-op when the boot console already attached. */
    fbcon_attach_late();
    /* Split the boot megapage map into per-section kernel permissions
     * (text ROX / rodata RO / data RW+NX, direct map NX).  Must run before
     * the first process page table copies the kernel half and before SMP
     * secondaries start sharing boot_pgdir. */
    arch_kernel_wx_finalize();
    timekeeping_vdso_init();
#ifdef CONFIG_SWAP
    swap_init();
#endif
    random_init();
    printf("[INIT] Random initialized\n");
    /* Randomise the stack canary as soon as the entropy pool is ready: the
     * boot stack holds only the kernel_main frame at this point and it never
     * returns, so swapping the guard cannot produce a false positive. */
    stack_protector_init();
    bootargs_init();
    klog_configure();
    printf("[INIT] Boot arguments parsed\n");
    /* The W^X policy depends on the cmdline (a20.wx=deny|strip|off), so this
     * must run after bootargs */
    mm_wx_policy_init();
    /* Same: the foreign-architecture translation channel is a cmdline
     * feature (a20.xlator=*), so it also needs bootargs parsed first.
     * Absent entirely in a CONFIG_XLATOR=n build. */
#ifdef CONFIG_XLATOR
    xlator_config_init();
#endif
#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
    /* 预标记上限同理走 cmdline（a20.anonprov=<pages>），须在 bootargs 之后 */
    mm_pt_anon_prov_init();
#endif
    driver_core_init();
    if (driver_memory_protect_boot())
        panic("boot: cannot protect RAM and firmware storage from MMIO");
    printf("[INIT] Driver core initialized\n");
    usb_core_init();
    printf("[INIT] USB core initialized\n");
#ifdef CONFIG_ACPI
    fw_node_t acpi_root = {0};
    const acpi_context_t *acpi = arch_acpi_context();
    if (acpi) {
        if (acpi_namespace_initialize(acpi, &acpi_root) < 0) {
            /* A firmware quirk must leave a boot log rather than a panic
             * before the persistent log is even mounted.  The board/static
             * fallback below still gets a chance to publish devices. */
            kerr("[ACPI] namespace initialization failed; continuing\n");
        } else if (!fw_node_valid(&boot_context_get()->hardware_root) &&
                   boot_context_set_hardware_root(&acpi_root) < 0) {
            kerr("[ACPI] cannot publish hardware root; continuing\n");
        }
    }
#endif
#if defined(CONFIG_X86_64) && defined(CONFIG_ACPI)
    (void)amd_iommu_firmware_report(arch_acpi_context());
#endif
    const fw_node_t *hardware = &boot_context_get()->hardware_root;
    if (fw_node_valid(hardware)) {
        if (firmware_device_enumerate(hardware) < 0)
            kerr("[FIRMWARE] enumeration incomplete; continuing with "
                 "already-discovered devices\n");
    } else if (current_board && current_board->enumerate_devices) {
        current_board->enumerate_devices();
        printf("[INIT] Static board fallback devices enumerated (%s)\n",
               current_board->name ? current_board->name : "unknown");
    }
    if (current_board && current_board->enumerate_buses)
        current_board->enumerate_buses();
#if defined(CONFIG_X86_64) && defined(CONFIG_ACPI)
    (void)amd_iommu_pci_report(arch_acpi_context());
#endif
    if (current_board && current_board->configure_devices)
        current_board->configure_devices();
    if (boot_framebuffer_enumerate() < 0)
        panic("boot: invalid framebuffer discovery");
    boot_advance(BOOT_PHASE_MEMORY, BOOT_PHASE_DEVICES);
    driver_probe_all();
    printf("[INIT] Drivers probed\n");
    usb_core_scan();
    printf("[INIT] USB devices scanned\n");
    /* PS/2 controller (x86_64) is owned by the ps2.a20drv drvmod module;
     * the built-in init was removed by the drvmod migration. */
#ifdef CONFIG_DRIVER_LIFECYCLE_TEST
    driver_lifecycle_test_run();
#endif
    arch_run_platform_selftests();
    /* Runs after driver_probe_all() so the IRQ registry already holds whatever
     * the board claimed, and before vfs_init(): this only touches the driver
     * core and the IRQ table. */
#ifdef CONFIG_PLATFORM_IRQ_TEST
    platform_irq_test_run();
#endif
    vfs_init();
    printf("[INIT] VFS initialized\n");
#ifdef CONFIG_DRIVER_DEPLOYMENT_GENERIC
    /* The root ramfs is available now. Load only early kernel modules before
     * mounting /bin; user-service packages still wait for proc_init(). */
    driver_manager_early_init();
#endif

    /* Storage and the persistent log come before the network stack.  The
     * block drivers are already probed, and a slow or hanging NIC probe must
     * not cost the boot log for everything that happens after this point.  In
     * the distro layout /extra must be mounted first so the log lands at
     * /extra/var/log/a20 and remains visible after chroot. */
#if defined(CONFIG_STORAGE_READ_ONLY)
    mount_read_only_storage();
#elif defined(CONFIG_RAMFS_USER)
    /* The boot card contains firmware payloads, not the RAM shell's /bin.
     * Keep embedded executables visible; data disks can be mounted explicitly. */
    printf("[INIT] RAMFS userspace: keeping embedded /bin\n");
#ifdef CONFIG_RAMFS_DISTRO
    /* Explicit ESP logging is independent of the desktop filesystem. Flush
     * the pre-mount evidence even if a USB read prevents /extra from mounting. */
    if (klog_mode() == KLOG_MODE_ESP) {
        mount_persistent_log();
        kinfo("[INIT] probing desktop root\n");
        klog_disk_sync();
    }
    mount_ramfs_distro();
    if (klog_mode() != KLOG_MODE_ESP)
        mount_persistent_log();
#else
    mount_persistent_log();
#endif
#ifdef CONFIG_RAMFS_MEDIA
    mount_ramfs_media();
#endif
#elif defined(BRINGUP)
    printf("[INIT] BRINGUP mode: no block devices\n");
#else
    mount_block_devices();
    /* Non-RAMFS profiles (external root, SD/FIT worlds) get the same
     * persistent log.  A dedicated "A20OS log" partition is preferred; the
     * mount code falls back to a writable rootfs directory. */
    mount_persistent_log();
#endif

    if (current_board && current_board->defer_network_init) {
        printf("[INIT] Network deferred by board recovery profile\n");
    } else {
        net_init();
        printf("[INIT] Network initialized\n");
    }

    proc_init();
    a20_i2c_hid_start_worker();
#ifdef CONFIG_ACPI
    acpi_os_start_worker();
#endif
    printf("[INIT] Process manager initialized\n");
#ifdef CONFIG_VMO_PAGE_TEST
    vmo_page_selftest_run();
#endif
#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
    a20_registry_init();
    printf("[INIT] Service registry initialized\n");
#endif
    smp_init();
    smp_boot_secondaries();
    arch_unmap_boot_identity();
    loop_init();

#if defined(BRINGUP) && !defined(CONFIG_RAMFS_USER)
    boot_advance(BOOT_PHASE_DEVICES, BOOT_PHASE_READY);
    printf("[INIT] System ready (bringup, no userspace)\n\n");
    hyp_boot_selftest();
    smp_selftest_boot();
    bringup_smoke_test();
#else
#ifdef CONFIG_COOPERATIVE_BOOT
    /* Bootstrap userspace directly from the idle context on boards using
     * cooperative scheduling during early bring-up. */
    printf("[INIT] bootstrapping userspace in cooperative mode\n");
#else
    int ret = proc_alloc(init_kthread);
    if (ret < 0)
        panic("Failed to create init_kthread");
    if (proc_alloc(oom_kswapd_thread) < 0)
        kerr("[KSWAPD] failed to spawn background reclaimer\n");
#if CONFIG_KLOG_PERSISTENT
    if (klog_disk_ready() && proc_alloc(klog_disk_thread) < 0)
        kerr("[LOG] failed to spawn persistent log writer\n");
#endif
    /* Cluster subsystem self-check (docs/zh/cluster/03-kernel-impl.md §5.6):
     * inert unless the command line carries clxselftest=1; costs one
     * strstr on normal boots.  Also calls a20_clx_core_init() when the
     * bootarg is present, so the check does not depend on a userspace
     * cluster syscall happening first. */
#ifndef CONFIG_NET_STACK_BSD
    {
        extern void a20_clx_selftest_boot(void);
        a20_clx_selftest_boot();
    }
#endif
#endif

    boot_advance(BOOT_PHASE_DEVICES, BOOT_PHASE_READY);
    klog_disk_mark_ready();
    printf("[INIT] System ready\n\n");
    printf("\033[1;36m");
    printf("%s\n",
           "                    :%%%%%%%.                                 ");
    printf("%s\n",
           "                    %%%%%%%*                                  ");
    printf("%s\n",
           "                   -%%%%%%%                                   ");
    printf("%s\n",
           "                   %%%%%%%-                                   ");
    printf("%s\n",
           "                  -%%%%%%%                                    ");
    printf("%s\n",
           "                  %%%%%%%=                                    ");
    printf("%s\n",
           "                 *%%%%%%%                                     ");
    printf("%s\n",
           "                .%%%%%%%:=+                                   ");
    printf("%s\n",
           "                @%%%%%%% %%                                   ");
    printf("%s\n",
           "                %%%%%%% +%%+                                  ");
    printf("%s\n",
           "               #%%%%%%+ %%%%                                  ");
    printf("%s\n",
           "              :%%%%%%% %%%%%-                                 ");
    printf("%s\n",
           "              %%%%%%%# %%%%%%    ........  .........          ");
    printf("%s\n",
           "             =%%%%%%% -%%%%%%-   ######### #########=         ");
    printf("%s\n",
           "             %%%%%%%=  %%%%%%#   ++*+*+*## #*++++++#=         ");
    printf("%s\n",
           "            *%%%%%%%   :%%%%%%:         ## #+      #=         ");
    printf("%s\n",
           "            %%%%%%%.    %%%%%%%         ## #+      #=         ");
    printf("%s\n",
           "           +%%%%%%%     =%%%%%%         *# #+      #=         ");
    printf("%s\n",
           "           %%%%%%%:      %%%%%%*  ######## #+      #=         ");
    printf("%s\n",
           "          %%%%%%%%       %%%%%%% :######## #+      #=         ");
    printf("%s\n",
           "         :%%%%%%%.        %%%%%%::#=       #+      #=         ");
    printf("%s\n",
           "         %%%%%%%* %%%%%%%%%%%%%%::#=       #+      #=         ");
    printf("%s\n",
           "        :%%%%%%% -%%%%%%%%%%%%%%::#=       #+      #=         ");
    printf("%s\n",
           "        %%%%%%%: %%%%%%%%%%%%%%%::######## #########=         ");
    printf("%s\n", "       :======= .===============. -------- ---------:    ");
    printf("\033[0m");
    printf("Welcome to A20OS!\n\n");
    printf("[INIT] entering scheduler...\n");

    if (current_board && current_board->prepare_scheduler)
        current_board->prepare_scheduler();

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

    driver_runtime_selftest_boot();
    arch_run_bootarg_selftests();
    a20_driver_boot_selftests();
    hyp_boot_selftest();

    /* Generic systems discover optional packages after the root filesystem is
     * available. Embedded deployments link all drivers into the kernel. */
#ifdef CONFIG_DRIVER_DEPLOYMENT_GENERIC
    driver_manager_init();
#endif
    /* Start the one-shot diagnostic before the network attach: a hung NIC or
     * protocol stack must not be able to take the final screen report with
     * it.  The thread sleeps first, so it does not race normal boot output. */
#if CONFIG_KLOG_ENABLED
    if (proc_alloc(boot_report_thread) < 0)
        kerr("[LOG] failed to spawn boot report thread\n");
#endif

    /* BSD starts a process-context executor here even when the NIC driver is
     * statically linked. Driver packaging must not decide whether it starts. */
    if (!current_board || !current_board->defer_network_init)
        net_stack_attach_interfaces();

    driver_runtime_selftest_runtime();

#ifdef CONFIG_BSD_BOOTSTRAP_TEST
    extern void a20_bsd_bootstrap_test(void);
    a20_bsd_bootstrap_test();
#endif

    const char *init_path = NULL;
    int fd = -1;
    char distro_interp[MAX_PATH_LEN];
    char distro_script[MAX_PATH_LEN];
    int is_distro_shebang = 0;

    int distro_via_embedded_init = 0;
#if defined(CONFIG_WIFI_RTW89) || defined(CONFIG_RAMFS_DESKTOP_TOOLS)
    /* Stage embedded radio/desktop tools in RAM before executing the
     * distribution's unmodified init; desktop staging is radio-independent. */
    distro_via_embedded_init = 1;
#endif
    if (!distro_via_embedded_init) {
        int marker_fd = vfs_open("/extra/etc/a20-distro", O_RDONLY, 0);
        if (marker_fd >= 0) {
            vfs_close(marker_fd);
            printf("[INIT] Distro rootfs detected (/extra/etc/a20-distro)\n");
            {
                /* Take the root as the (mount, vnode) pair it is, the same
                 * way chroot(2) does, so every later decision -- umount
                 * busy checks, pivot_root, confinement -- sees an object
                 * rather than a string it has to re-parse. */
                vnode_t *distro_root = vfs_resolve("/extra");
                if (distro_root) {
                    vfs_task_root_set(cur, distro_root->mnt, distro_root,
                                      "/extra");
                    vnode_put(distro_root);
                } else {
                    strncpy(cur->fs.root_path, "/extra", MAX_PATH_LEN - 1);
                    cur->fs.root_path[MAX_PATH_LEN - 1] = '\0';
                }
            }
            printf("[INIT] Chrooted to /extra\n");

            int init_fd = vfs_open("/sbin/init", O_RDONLY, 0);
            if (init_fd >= 0) {
                char magic[2];
                int n = vfs_read(init_fd, magic, 2);
                if (n == 2 && magic[0] == '#' && magic[1] == '!') {
                    char buf[256];
                    vfs_lseek(init_fd, 0, SEEK_SET);
                    n = vfs_read(init_fd, buf, sizeof(buf) - 1);
                    if (n > 0) {
                        buf[n] = '\0';
                        char *cp = buf + 2;
                        while (*cp == ' ' || *cp == '\t')
                            cp++;
                        char *end = cp;
                        while (*end && *end != ' ' && *end != '\t' &&
                               *end != '\n' && *end != '\r')
                            end++;
                        *end = '\0';
                        strncpy(distro_interp, cp, MAX_PATH_LEN - 1);
                        distro_interp[MAX_PATH_LEN - 1] = '\0';
                        strncpy(distro_script, "/sbin/init", MAX_PATH_LEN - 1);
                        is_distro_shebang = 1;
                        printf("[INIT] Distro shebang: %s %s\n", distro_interp,
                               distro_script);
                    }
                }
                vfs_close(init_fd);
            }

            init_path = is_distro_shebang ? distro_interp : "/sbin/init";
            printf("[INIT] opening %s (distro mode)...\n", init_path);
            fd = vfs_open(init_path, O_RDONLY, 0);
            if (fd < 0)
                panic("init: distro %s not found: %d\n", init_path, fd);
        }
    }

    if (!init_path) {
#ifdef CONFIG_EXTERNAL_ROOT
        init_path = "/a20/init";
#else
        init_path = "/bin/init";
#endif
        printf("[INIT] opening %s...\n", init_path);
        fd = vfs_open(init_path, O_RDONLY, 0);
        if (fd < 0) {
            printf("[INIT] Cannot open %s: %d\n", init_path, fd);
            init_path = "/init";
            fd = vfs_open(init_path, O_RDONLY, 0);
            if (fd < 0) {
                panic("init: no init program found");
            }
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
        printf("[INIT] image: size=%lu fnv1a64=%016lx class=%u machine=%u "
               "file-entry=0x%lx\n",
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
        mm_seg_t *entry_vma = info.mmap;
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
    char *init_argv[4] = {NULL};
    int init_argc = 1;
    if (is_distro_shebang) {
        init_argv[0] = distro_interp;
        init_argv[1] = distro_script;
        init_argc = 2;
    } else {
        init_argv[0] = (char *)init_path;
    }
    /* The initial user image keeps the syscall time path (no vDSO); what it execs
     * gets the vDSO through the regular exec path (proc/exec.c). */
    uintptr_t init_auxv[A20_AUXV_MAX_PAIRS][2];
    uint32_t init_auxv_n = 0;
    uint64_t user_sp = elf_setup_stack(info.stack_top, init_argc, init_argv,
                                       NULL, &info, 0, init_auxv, &init_auxv_n);
    if (user_sp == 0) {
        panic("init: elf_setup_stack failed");
    }
    printf("[INIT] user_sp=0x%lx\n", (unsigned long)user_sp);

    size_t init_total_vm = 0;
    for (mm_seg_t *v = info.mmap; v; v = v->next)
        init_total_vm += (v->end - v->start) / PAGE_SIZE;

    ret = proc_alloc_user_image(info.entry, user_sp, info.pgdir, info.mmap,
                                info.brk, info.stack_top, init_total_vm,
                                info.tls_tp
#ifdef CONFIG_NOMMU
                                ,
                                info.nommu_allocs, info.nommu_alloc_sizes,
                                info.nommu_alloc_types, info.num_nommu_allocs
#endif
                                ,
                                PROC_USER_IMAGE_BOOT_INIT | PROC_USER_IMAGE_DEFER_READY);
    if (ret != 1) {
        panic("init: PID 1 creation failed: %d\n", ret);
    }

    /* Complete loader-owned state before PID1 becomes runnable. */
    task_t *init_task = proc_find_get(ret);
    if (!init_task || !init_task->mm)
        panic("init: missing initial address space");
    init_task->mm->auxv_n = init_auxv_n;
    for (uint32_t i = 0; i < init_auxv_n && i < A20_AUXV_MAX_PAIRS; i++) {
        init_task->mm->auxv[i][0] = init_auxv[i][0];
        init_task->mm->auxv[i][1] = init_auxv[i][1];
    }

#ifdef CONFIG_NOMMU
    arch_flush_icache_range((const void *)info.load_addr, info.load_size);
#else
    /* map_segment() cleaned executable pages through their direct-map alias;
     * discard any stale instruction lines before this image first runs. */
    arch_fence_i();
#endif

    printf("[INIT] user init created: pid=%d entry=0x%lx sp=0x%lx\n", ret,
           (unsigned long)info.entry, (unsigned long)user_sp);

    proc_publish_deferred_task(init_task);
    proc_put(init_task);

#ifdef CONFIG_COOPERATIVE_BOOT
    /* PID 0 remains the scheduler host; userspace PID1 reaps its children. */
    printf("[INIT] entering cooperative idle loop\n");
    idle_loop();
#endif

    /* The upstream userspace PID1 is the reaper. The bootstrap thread has no
     * permanent userspace role; exiting also avoids an ECHILD/yield busy loop. */
    proc_exit(0);
}
