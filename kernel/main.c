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
#include "core/string.h"
#include "core/consts.h"
#include "core/defs.h"
#include "core/panic.h"
#include "core/timekeeping.h"
#include "core/random.h"
#include "fs/vfs.h"
#include "drivers/block/virtio_blk.h"
#include "drivers/block/virtio_scsi.h"
#ifdef CONFIG_X86_64
#include "drivers/block/ahci.h"
#endif
#include "drivers/gpu/virtio_gpu.h"
#include "drivers/input/virtio_input.h"
#ifdef CONFIG_X86_64
#include "drivers/input/ps2.h"
#endif
#include "fs/block_cache.h"
#include "core/klog.h"
#include "proc/signal.h"
#include "drivers/block/loop.h"
#include "net/socket.h"
#include "drivers/core/driver_core.h"
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

#ifndef BRINGUP
typedef struct {
    block_dev_t block;
    block_dev_t *parent;
    uint64_t first_lba;
    uint64_t sectors;
} partition_block_dev_t;

static int partition_read_sector(block_dev_t *block, uint64_t lba, void *buf,
                                 size_t count) {
    partition_block_dev_t *part = (partition_block_dev_t *)block->priv;
    if (!part || !part->parent || lba > part->sectors ||
        count > part->sectors - lba)
        return -1;
    return part->parent->read_sector(part->parent, part->first_lba + lba, buf, count);
}

static int partition_write_sector(block_dev_t *block, uint64_t lba, const void *buf,
                                  size_t count) {
    partition_block_dev_t *part = (partition_block_dev_t *)block->priv;
    if (!part || !part->parent || lba > part->sectors ||
        count > part->sectors - lba)
        return -1;
    return part->parent->write_sector(part->parent, part->first_lba + lba, buf, count);
}

/* The VBox UEFI image is GPT-partitioned.  Expose its first partition to the
 * existing FAT/ext4 mount code instead of assuming a superfloppy image. */
static block_dev_t *first_gpt_partition(block_dev_t *parent) {
    static partition_block_dev_t partition;
    uint8_t entry[128];
    uint8_t header[512];
    if (!parent || !parent->read_sector || parent->read_sector(parent, 1, header, 1) != 0)
        return NULL;
    if (memcmp(header, "EFI PART", 8) != 0)
        return NULL;

    uint64_t entries_lba = 0;
    for (int i = 0; i < 8; i++)
        entries_lba |= (uint64_t)header[72 + i] << (i * 8);
    uint32_t entry_size = (uint32_t)header[84] | ((uint32_t)header[85] << 8) |
                          ((uint32_t)header[86] << 16) | ((uint32_t)header[87] << 24);
    if (!entries_lba || entry_size < sizeof(entry) || entry_size > 512 ||
        parent->read_sector(parent, entries_lba, entry, 1) != 0)
        return NULL;

    uint64_t first = 0, last = 0;
    for (int i = 0; i < 8; i++) {
        first |= (uint64_t)entry[32 + i] << (i * 8);
        last |= (uint64_t)entry[40 + i] << (i * 8);
    }
    if (!first || last < first || last >= parent->capacity)
        return NULL;
    partition.parent = parent;
    partition.first_lba = first;
    partition.sectors = last - first + 1;
    partition.block.read_sector = partition_read_sector;
    partition.block.write_sector = partition_write_sector;
    partition.block.capacity = partition.sectors;
    partition.block.sector_size = parent->sector_size;
    partition.block.priv = &partition;
    return &partition.block;
}

static int try_mount(block_dev_t *dev, const char *mnt, const char *fstype) {
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
        printf("[INIT] Block device -> %s (%s)\n", mnt, fstype);
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

        if (!bin_ok && try_mount(virtio_blk_get_dev(i), "/bin", "fat32") == 0) {
            bin_ok = 1;
            continue;
        }
        if (!test_ok && try_mount(virtio_blk_get_dev(i), "/test", "ext4") == 0) {
            test_ok = 1;
            continue;
        }
    }

    /* VirtualBox ARM exposes its boot disk through a VirtIO-SCSI controller,
     * not a VirtIO block function. The controller driver is bound during PCI
     * enumeration, so mount any discovered LUNs alongside virtio-blk disks. */
    for (int i = 0; i < 4; i++) {
        block_dev_t *scsi = virtio_scsi_get_dev(i);
        if (!scsi)
            continue;
        block_dev_t *fsdev = first_gpt_partition(scsi);
        if (!fsdev)
            fsdev = scsi;
        if (!bin_ok && try_mount(fsdev, "/bin", "fat32") == 0) {
            bin_ok = 1;
            continue;
        }
        if (!test_ok && try_mount(fsdev, "/test", "ext4") == 0)
            test_ok = 1;
    }

    block_dev_t *ahci = NULL;
#ifdef CONFIG_X86_64
    ahci = ahci_get_dev(0);
#endif
    if (ahci) {
        if (!bin_ok)
            bin_ok = try_mount(ahci, "/bin", "fat32") == 0;
        if (!test_ok)
            test_ok = try_mount(ahci, "/test", "ext4") == 0;
    }

    if (!bin_ok)  printf("[INIT] WARNING: no FAT32 device for /bin\n");
    if (!test_ok) printf("[INIT] no ext4 device for /test (ok without sdcard)\n");
}
#endif /* BRINGUP */

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
#ifdef CONFIG_SWAP
    swap_init();
#endif
    random_init();
    printf("[INIT] Random initialized\n");
    bootargs_init();
    printf("[INIT] Boot arguments parsed\n");
    driver_core_init();
    printf("[INIT] Driver core initialized\n");
    if (current_board && current_board->enumerate_devices) {
        current_board->enumerate_devices();
        printf("[INIT] Board devices enumerated (%s)\n",
               current_board->name ? current_board->name : "unknown");
    }
    driver_probe_all();
    printf("[INIT] Drivers probed\n");
    virtio_gpu_init();
    virtio_input_init();
#ifdef CONFIG_X86_64
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
    arch_unmap_boot_identity();
    loop_init();

#ifdef BRINGUP
    printf("[INIT] System ready (bringup, no userspace)\n\n");
    bringup_smoke_test();
#else
#ifdef CONFIG_BOARD_VIRTUALBOX_AARCH64
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

#ifdef CONFIG_BOARD_VIRTUALBOX_AARCH64
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

    printf("[INIT] loading ELF...\n");
    elf_load_info_t info;
    int ret = elf_load(fd, init_path, &info);
    vfs_close(fd);

    if (ret < 0) {
        panic("init: ELF load failed: %d\n", ret);
    }

    printf("[INIT] ELF loaded: entry=0x%lx stack=0x%lx\n",
           (unsigned long)info.entry, (unsigned long)info.stack_top);

    /* Set up the initial user stack with argc/argv/envp/auxv so the
     * C runtime (crt1.o / musl) finds a valid stack layout.  Without
     * this, __libc_start_main dereferences garbage and the init
     * process crashes silently before ever reaching main().
     */
    char *init_argv[] = { (char *)init_path, NULL };
    uint64_t user_sp = elf_setup_stack(info.stack_top, 1, init_argv, NULL, &info);
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
                                );
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

#ifdef CONFIG_BOARD_VIRTUALBOX_AARCH64
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
