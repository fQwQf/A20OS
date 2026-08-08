/*
 * A20OS unified driver manager.
 *
 * The single authority for optional driver discovery and activation.
 * It reads the .a20drv descriptor (the only driver metadata) from every
 * package in the DriverStore and routes each one by placement:
 *
 *   - kernel-module : loaded into the kernel direct-map by the drvmod
 *                     loader; the module registers a unified driver_t and
 *                     binds through the driver core's normal match/probe
 *                     path.
 *   - user-service  : spawned as a native user process that claims the
 *                     device through the udriver framework (one owner per
 *                     device; the core lets only read-only kernel probes
 *                     bind user-owned devices).
 *
 * Board-owned devices that modules or user services bind to are registered
 * here as unified platform devices.
 */

#include "core/types.h"
#include "core/defs.h"
#include "core/consts.h"
#include "core/klog.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/errno.h"
#include "proc/proc.h"
#include "mm/elf.h"
#include "fs/vfs.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/udriver.h"
#include "drivers/bus/platform_bus.h"
#include "drvmod/drvmod.h"

#ifdef CONFIG_ABI_NATIVE
#include "abi/native/types.h"
#include "abi/native/rights.h"
#include "ipc/handle_table.h"
#endif

/* Generic keeps the root-device substrate built in until an initramfs exists.
 * Once present, /boot/drivers is the early package store; /bin/lib/drivers is
 * the runtime store. Both are ordinary .a20drv directories and are scanned by
 * this manager, so there is no second manifest or activation path. */
#define DRIVER_STORE_EARLY    "/boot/drivers"
#define DRIVER_STORE_RUNTIME  "/bin/lib/drivers"
#define DRIVER_STORE_MAX      64

/* ------------------------------------------------------------------ */
/*  Descriptor reading (ELF .a20drv section)                          */
/* ------------------------------------------------------------------ */

#define DRIVER_MGR_SHT_PROGBITS 1

int driver_descriptor_read(int fd, a20_driver_descriptor_t *out)
{
    if (!out || fd < 0)
        return -EINVAL;
    if (vfs_lseek(fd, 0, SEEK_SET) < 0)
        return -EIO;

    Elf64_Ehdr eh;
    int r = vfs_read(fd, (char *)&eh, sizeof(eh));
    if (r < (int)sizeof(eh))
        return -ENOEXEC;
    if (*(uint32_t *)eh.e_ident != ELF_MAGIC)
        return -ENOEXEC;
    if (eh.e_ident[4] != ELFCLASS64)
        return -ENOEXEC;
    if (eh.e_shentsize != sizeof(Elf64_Shdr) || !eh.e_shnum ||
        eh.e_shnum > 128 || eh.e_shstrndx >= eh.e_shnum)
        return -ENOEXEC;

    Elf64_Shdr shdrs[128];
    size_t shdr_bytes = (size_t)eh.e_shnum * sizeof(Elf64_Shdr);
    if (vfs_pread(fd, (char *)shdrs, shdr_bytes, eh.e_shoff) !=
        (int)shdr_bytes)
        return -ENOEXEC;

    Elf64_Shdr *shstr = &shdrs[eh.e_shstrndx];
    if (!shstr->sh_size || shstr->sh_size > 4096)
        return -ENOEXEC;
    char names[4096];
    if (vfs_pread(fd, names, shstr->sh_size, shstr->sh_offset) !=
        (int)shstr->sh_size)
        return -ENOEXEC;

    for (int i = 0; i < eh.e_shnum; i++) {
        Elf64_Shdr *sh = &shdrs[i];
        if (sh->sh_name >= shstr->sh_size)
            continue;
        if (strcmp(names + sh->sh_name, ".a20drv") != 0)
            continue;
        if (sh->sh_type != DRIVER_MGR_SHT_PROGBITS ||
            sh->sh_size != sizeof(*out))
            return -ENOEXEC;
        if (vfs_pread(fd, (char *)out, sizeof(*out), sh->sh_offset) !=
            (int)sizeof(*out))
            return -ENOEXEC;
        if (out->magic != A20_DRIVER_DESCRIPTOR_MAGIC ||
            out->version != A20_DRIVER_DESCRIPTOR_VERSION ||
            out->match_count > A20_DRIVER_MAX_MATCH ||
            !out->name[0])
            return -ENOEXEC;
        return 0;
    }
    return -ENOEXEC;
}

/* ------------------------------------------------------------------ */
/*  Module-owned device registration                                  */
/* ------------------------------------------------------------------ */

/* Platform devices that optional kernel modules or user services bind
 * to.  The driver core uses user_owned to enforce one owner per device:
 * only read-only kernel probes bind a user-owned device. */
static platform_device_t g_rtc_pdev;
static platform_device_t g_blk_pdev;
static platform_device_t g_vinput_pdev;
#if defined(CONFIG_X86_64)
static platform_device_t g_ps2_pdev;
static platform_device_t g_tpm_pdev;
#endif

static void manager_register_board_devices(void)
{
#if defined(CONFIG_BOARD_QEMU_VIRT_RISCV64) || \
    defined(CONFIG_BOARD_QEMU_VIRT_AARCH64) || \
    defined(CONFIG_BOARD_QEMU_VIRT_LOONGARCH64)
    /* goldfish RTC (dual-placement: kernel probe module + user rtcd). */
    static resource_t rtc_res[] = {
        { RES_MMIO, 0x101000, 0x1010FF, IORESOURCE_MMIO_32BIT, "rtc" },
    };
    memset(&g_rtc_pdev, 0, sizeof(g_rtc_pdev));
    g_rtc_pdev.dev.name = "goldfish-rtc";
    g_rtc_pdev.dev.res = rtc_res;
    g_rtc_pdev.dev.res_count = 1;
    g_rtc_pdev.dev.user_owned = 1;
    g_rtc_pdev.id.vendor = 0x101000;
    g_rtc_pdev.id.device = 0;
    g_rtc_pdev.id.subvendor = VENDOR_ANY;
    g_rtc_pdev.id.subdevice = DEVICE_ANY;
    platform_device_register(&g_rtc_pdev);
#endif

#if defined(CONFIG_BOARD_QEMU_VIRT_RISCV64)
    /* User-reserved virtio-blk slot 3 (user service ubd). */
    static resource_t blk_res[] = {
        { RES_MMIO, 0x10004000, 0x10004FFF, IORESOURCE_MMIO_32BIT, "ubd" },
    };
    memset(&g_blk_pdev, 0, sizeof(g_blk_pdev));
    g_blk_pdev.dev.name = "virtio-blk-user";
    g_blk_pdev.dev.res = blk_res;
    g_blk_pdev.dev.res_count = 1;
    g_blk_pdev.dev.user_owned = 1;
    g_blk_pdev.id.vendor = 0x10004000;
    g_blk_pdev.id.device = 0;
    g_blk_pdev.id.subvendor = VENDOR_ANY;
    g_blk_pdev.id.subdevice = DEVICE_ANY;
    platform_device_register(&g_blk_pdev);
#endif

#if defined(CONFIG_BOARD_QEMU_VIRT_RISCV64)
    /* Dual-placement virtio-input slot 5 (kernel probe + user uinputd). */
    static resource_t vinput_res[] = {
        { RES_MMIO, 0x10006000, 0x10006FFF, IORESOURCE_MMIO_32BIT, "vinput" },
    };
    memset(&g_vinput_pdev, 0, sizeof(g_vinput_pdev));
    g_vinput_pdev.dev.name = "virtio-input-slot5";
    g_vinput_pdev.dev.res = vinput_res;
    g_vinput_pdev.dev.res_count = 1;
    g_vinput_pdev.dev.user_owned = 1;
    g_vinput_pdev.id.vendor = 0x10006000;
    g_vinput_pdev.id.device = 0;
    g_vinput_pdev.id.subvendor = VENDOR_ANY;
    g_vinput_pdev.id.subdevice = DEVICE_ANY;
    platform_device_register(&g_vinput_pdev);
#elif defined(CONFIG_BOARD_QEMU_VIRT_AARCH64)
    static resource_t vinput_res[] = {
        { RES_MMIO, 0x0A000A00, 0x0A000BFF, IORESOURCE_MMIO_32BIT, "vinput" },
    };
    memset(&g_vinput_pdev, 0, sizeof(g_vinput_pdev));
    g_vinput_pdev.dev.name = "virtio-input-slot5";
    g_vinput_pdev.dev.res = vinput_res;
    g_vinput_pdev.dev.res_count = 1;
    g_vinput_pdev.dev.user_owned = 1;
    g_vinput_pdev.id.vendor = 0x0A000A00;
    g_vinput_pdev.id.device = 0;
    g_vinput_pdev.id.subvendor = VENDOR_ANY;
    g_vinput_pdev.id.subdevice = DEVICE_ANY;
    platform_device_register(&g_vinput_pdev);
#endif

#if defined(CONFIG_X86_64)
    memset(&g_ps2_pdev, 0, sizeof(g_ps2_pdev));
    g_ps2_pdev.dev.name = "ps2";
    g_ps2_pdev.id.vendor = 0x50533200;
    g_ps2_pdev.id.device = 0;
    g_ps2_pdev.id.subvendor = VENDOR_ANY;
    g_ps2_pdev.id.subdevice = DEVICE_ANY;
    platform_device_register(&g_ps2_pdev);

    memset(&g_tpm_pdev, 0, sizeof(g_tpm_pdev));
    g_tpm_pdev.dev.name = "tpm";
    g_tpm_pdev.id.vendor = 0x54504D00;
    g_tpm_pdev.id.device = 0;
    g_tpm_pdev.id.subvendor = VENDOR_ANY;
    g_tpm_pdev.id.subdevice = DEVICE_ANY;
    platform_device_register(&g_tpm_pdev);
#endif
}

/* ------------------------------------------------------------------ */
/*  User-service activation                                           */
/* ------------------------------------------------------------------ */

#ifdef CONFIG_ABI_NATIVE

static struct a20_ht_internal *manager_ht_create(void);
static struct a20_ht_internal *manager_ht_create(void)
{
    extern struct a20_ht_internal *a20_ht_create(void);
    return a20_ht_create();
}

static int64_t manager_handle_install(struct a20_ht_internal *ht, void *obj,
                                      uint16_t type, a20_rights_t rights)
{
    extern int64_t a20_handle_install(struct a20_ht_internal *ht, void *object,
                                      uint16_t type, a20_rights_t rights);
    return a20_handle_install(ht, obj, type, rights);
}

int driver_manager_spawn_user(const char *path)
{
    if (!path)
        return -EINVAL;

    int fd = vfs_open(path, O_RDONLY, 0);
    if (fd < 0)
        return fd;
    elf_load_info_t info;
    memset(&info, 0, sizeof(info));
    int r = elf_load(fd, path, &info);
    vfs_close(fd);
    if (r < 0)
        return r;
    if (!info.is_native_abi) {
        elf_load_info_discard(&info);
        return -EINVAL;
    }

    size_t total_vm = 0;
    for (vm_area_t *v = info.mmap; v; v = v->next)
        total_vm += (v->end - v->start) / PAGE_SIZE;

    int pid = proc_alloc_user_image(info.entry, info.stack_top, info.pgdir,
                                    info.mmap, info.brk, info.stack_top,
                                    total_vm, info.tls_tp
#ifdef CONFIG_NOMMU
                                    , info.nommu_allocs,
                                    info.nommu_alloc_sizes,
                                    info.nommu_alloc_types,
                                    info.num_nommu_allocs
#endif
                                    , 1);
    if (pid < 0) {
        elf_load_info_discard(&info);
        return pid;
    }

    task_t *task = proc_find_get(pid);
    if (!task)
        return -ENOMEM;
    task->abi_mode = 1;

    struct a20_ht_internal *ht = manager_ht_create();
    if (!ht) {
        proc_force_exit(task, 1);
        proc_publish_deferred_task(task);
        proc_put(task);
        return -ENOMEM;
    }
    __atomic_store_n(&task->a20_ht, ht, __ATOMIC_RELEASE);

    uint32_t stdin_h = 0xFFFFFFFF, stdout_h = 0xFFFFFFFF,
             stderr_h = 0xFFFFFFFF;
    int console_rd = vfs_open("/dev/console", O_RDONLY, 0);
    if (console_rd >= 0) {
        int64_t h = manager_handle_install(
            ht, (void *)(uintptr_t)console_rd, A20_OBJ_FILE,
            A20_RIGHT_READ | A20_RIGHT_SEEK | A20_RIGHT_DUP);
        if (h >= 0)
            stdin_h = (uint32_t)h;
    }
    int console_wr = vfs_open("/dev/console", O_WRONLY, 0);
    if (console_wr >= 0) {
        int64_t h = manager_handle_install(
            ht, (void *)(uintptr_t)console_wr, A20_OBJ_FILE,
            A20_RIGHT_WRITE | A20_RIGHT_SEEK | A20_RIGHT_DUP);
        if (h >= 0)
            stdout_h = (uint32_t)h;
    }
    int console_wr2 = vfs_open("/dev/console", O_WRONLY, 0);
    if (console_wr2 >= 0) {
        int64_t h = manager_handle_install(
            ht, (void *)(uintptr_t)console_wr2, A20_OBJ_FILE,
            A20_RIGHT_WRITE | A20_RIGHT_SEEK | A20_RIGHT_DUP);
        if (h >= 0)
            stderr_h = (uint32_t)h;
    }

    a20_handle_t self_h = 0;
    {
        int64_t h = manager_handle_install(
            ht, (void *)(uintptr_t)pid, A20_OBJ_TASK,
            A20_RIGHT_WAIT | A20_RIGHT_SIGNAL | A20_RIGHT_STAT |
            A20_RIGHT_DUP);
        if (h >= 0)
            self_h = (a20_handle_t)h;
    }

    uint32_t root_h = 0, cwd_h = 0;
    int root_fd = vfs_open("/", O_RDONLY, 0);
    if (root_fd >= 0) {
        int64_t h = manager_handle_install(
            ht, (void *)(uintptr_t)root_fd, A20_OBJ_DIRECTORY,
            A20_RIGHT_READ | A20_RIGHT_STAT | A20_RIGHT_DUP |
            A20_RIGHT_TRANSFER);
        if (h >= 0) {
            root_h = (uint32_t)h;
            vfs_ref_fd(root_fd);
            int64_t h2 = manager_handle_install(
                ht, (void *)(uintptr_t)root_fd, A20_OBJ_DIRECTORY,
                A20_RIGHT_READ | A20_RIGHT_STAT | A20_RIGHT_DUP);
            if (h2 >= 0)
                cwd_h = (uint32_t)h2;
            if (!cwd_h)
                cwd_h = root_h;
        }
    }

    uint32_t registry_h = 0;
    {
        extern int64_t a20_registry_install_client(struct a20_ht_internal *ht);
        int64_t h = a20_registry_install_client(ht);
        if (h >= 0)
            registry_h = (uint32_t)h;
    }

    char *argv[] = { (char *)path, NULL };
    uint64_t sp = elf_setup_stack_a20(info.stack_top, 1, argv, NULL, &info,
                                      stdin_h, stdout_h, stderr_h,
                                      (uint32_t)self_h, root_h, cwd_h, 0,
                                      registry_h);
    if (sp == 0) {
        proc_force_exit(task, 1);
        proc_publish_deferred_task(task);
        proc_put(task);
        return -EINVAL;
    }

    trap_context_t *trap = task->trap_ctx;
    TRAP_CTX_SP(trap) = sp;
    TRAP_CTX_SET_ARG0(trap, sp);

    proc_publish_deferred_task(task);
    proc_put(task);
    kinfo("[DRIVERMGR] spawned user-service driver '%s' pid=%d\n", path, pid);
    return pid;
}

#else /* !CONFIG_ABI_NATIVE */

int driver_manager_spawn_user(const char *path)
{
    (void)path;
    return -EOPNOTSUPP;
}

#endif /* CONFIG_ABI_NATIVE */

/* ------------------------------------------------------------------ */
/*  Store scan and activation                                         */
/* ------------------------------------------------------------------ */

static void manager_activate_path(const char *path, const char *name,
                                  int allow_user_services)
{
    int fd = vfs_open(path, O_RDONLY, 0);
    if (fd < 0)
        return;
    a20_driver_descriptor_t desc;
    int r = driver_descriptor_read(fd, &desc);
    if (r < 0) {
        kinfo("[DRIVERMGR] %s: invalid descriptor (%d)\n", path, r);
        vfs_close(fd);
        return;
    }
    switch (desc.placement) {
    case A20_DRIVER_PLACEMENT_KERNEL_MODULE: {
        int mid = drvmod_load(fd, name);
        if (mid < 0)
            kinfo("[DRIVERMGR] %s: module load failed (%d)\n", path, mid);
        break;
    }
    case A20_DRIVER_PLACEMENT_USER_SERVICE: {
        if (!allow_user_services) {
            kinfo("[DRIVERMGR] %s: deferred user-service package\n", path);
            break;
        }
        if (desc.flags & A20_DRIVER_FLAG_SUPERVISED) {
            kinfo("[DRIVERMGR] %s: lifecycle supervised externally, "
                  "recorded only\n", path);
            break;
        }
        int present = 0;
        for (uint32_t i = 0; i < desc.match_count; i++)
            if (udriver_window_present(desc.match[i].vendor)) {
                present = 1;
                break;
            }
        if (present) {
            int pid = driver_manager_spawn_user(path);
            if (pid < 0)
                kinfo("[DRIVERMGR] %s: user service spawn failed (%d)\n",
                      path, pid);
        } else {
            kinfo("[DRIVERMGR] %s: device absent, user service idle\n", path);
        }
        break;
    }
    default:
        kinfo("[DRIVERMGR] %s: unknown placement %u\n", path, desc.placement);
    }
    vfs_close(fd);
}

int driver_manager_activate(const char *store_path)
{
    if (!store_path)
        return -EINVAL;
    const char *name = store_path;
    for (const char *p = store_path; *p; p++)
        if (*p == '/')
            name = p + 1;
    manager_activate_path(store_path, name, 1);
    drvmod_init_all();
    return 0;
}

static void manager_scan_store(const char *store, int optional,
                               int allow_user_services)
{
    int dfd = vfs_open(store, O_RDONLY, 0);
    if (dfd < 0) {
        if (!optional)
            kinfo("[DRIVERMGR] store %s not found (%d)\n", store, dfd);
        return;
    }
    char dents[512];
    int scanned = 0;
    for (;;) {
        int n = vfs_getdents64(dfd, dents, sizeof(dents));
        if (n <= 0)
            break;
        for (int off = 0; off + (int)offsetof(vfs_dirent64_t, d_name) < n; ) {
            vfs_dirent64_t *de = (vfs_dirent64_t *)(dents + off);
            if (de->d_reclen < (int)offsetof(vfs_dirent64_t, d_name) ||
                off + de->d_reclen > n)
                break;
            const char *nm = de->d_name;
            size_t nlen = strlen(nm);
            if (nlen > 7 && strcmp(nm + nlen - 7, ".a20drv") == 0) {
                char path[128];
                snprintf(path, sizeof(path), "%s/%s", store, nm);
                manager_activate_path(path, nm, allow_user_services);
                if (++scanned >= DRIVER_STORE_MAX)
                    goto out;
            }
            off += de->d_reclen;
        }
    }
out:
    vfs_close(dfd);
}

void driver_manager_early_init(void)
{
    kinfo("[DRIVERMGR] early driver store init\n");
    manager_scan_store(DRIVER_STORE_EARLY, 1, 0);
    drvmod_init_all();
    kinfo("[DRIVERMGR] early driver store init done\n");
}

void driver_manager_init(void)
{
    kinfo("[DRIVERMGR] runtime driver manager init\n");
    manager_register_board_devices();
    manager_scan_store(DRIVER_STORE_RUNTIME, 0, 1);
    drvmod_init_all();
    kinfo("[DRIVERMGR] runtime driver manager init done\n");
}
