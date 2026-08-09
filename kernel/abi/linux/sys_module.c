#include "syscall_impl.h"

#include "drvmod/drvmod.h"
#include "fs/anonfd.h"
#include "fs/file.h"
#include "fs/vfs.h"
#include "mm/slab.h"

/*
 * Linux module syscalls mapped onto the A20OS driver-module (drvmod) loader.
 *
 * A20OS is a statically-linked kernel that loads relocatable driver modules
 * (ET_REL, `.a20drv` descriptor) through kernel/drvmod/loader.c.  The Linux
 * init_module(2)/finit_module(2)/delete_module(2) surface is provided here so
 * a privileged supervisor can drive the same module store; the loaded unit is
 * an A20 driver module, not a Linux kernel module.  The module image is staged
 * into an anonymous file so drvmod_load() can read it through the VFS.
 */

#define MODULE_MAX_SIZE  DRV_MOD_MAX_SIZE
#define MODULE_NAME_MAX  DRV_MOD_MAX_NAME

static int module_requires_priv(void)
{
    task_t *t = proc_current();
    if (!t)
        return -EPERM;
    if (!proc_has_cap(t, CAP_SYS_MODULE) && t->cred.euid != 0)
        return -EPERM;
    return 0;
}

/* Stage @len bytes from user memory @umod into an anonymous file and return
 * its global VFS fd (negative errno on failure). */
static int module_stage_image(const void *umod, unsigned long len)
{
    if (!umod || len == 0)
        return -EINVAL;
    if (len > MODULE_MAX_SIZE)
        return -E2BIG;

    int gfd = anonfd_create(0);
    if (gfd < 0)
        return gfd;

    char *kbuf = proc_scratch_buffer(65536);
    if (!kbuf) {
        vfs_close(gfd);
        return -ENOMEM;
    }
    unsigned long done = 0;
    while (done < len) {
        unsigned long chunk = len - done;
        if (chunk > 65536)
            chunk = 65536;
        if (copy_from_user(kbuf, (const char *)umod + done, chunk) < 0) {
            vfs_close(gfd);
            return -EFAULT;
        }
        if (vfs_write(gfd, kbuf, chunk) < 0) {
            vfs_close(gfd);
            return -EIO;
        }
        done += chunk;
    }
    if (vfs_lseek(gfd, 0, SEEK_SET) < 0) {
        vfs_close(gfd);
        return -EIO;
    }
    return gfd;
}

int64_t sys_init_module(const void *umod, unsigned long len, const char *uargs)
{
    (void)uargs;
    int priv = module_requires_priv();
    if (priv < 0)
        return priv;

    int gfd = module_stage_image(umod, len);
    if (gfd < 0)
        return gfd;

    int mid = drvmod_load(gfd, "init_module");
    vfs_close(gfd);
    if (mid < 0)
        return mid;
    return 0;
}

int64_t sys_finit_module(int fd, const char *uargs, int flags)
{
    (void)uargs;
    int priv = module_requires_priv();
    if (priv < 0)
        return priv;
    if (flags & ~(0x1 /* MODULE_INIT_IGNORE_MODVERSIONS */ |
                  0x2 /* MODULE_INIT_IGNORE_VERMAGIC */ |
                  0x4 /* MODULE_INIT_COMPRESSED_FILE */))
        return -EINVAL;

    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0)
        return gfd;

    vfile_t *vf = vfs_get_file_ref((int)gfd);
    if (!vf)
        return -EBADF;
    if (vf->flags == O_WRONLY) {
        vfs_put_file_ref((int)gfd, vf);
        return -EINVAL;
    }
    char name[MODULE_NAME_MAX];
    strncpy(name, vf->path[0] ? vf->path : "finit_module", sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    vfs_put_file_ref((int)gfd, vf);

    if (vfs_lseek((int)gfd, 0, SEEK_SET) < 0)
        return -EIO;
    int mid = drvmod_load((int)gfd, name);
    if (mid < 0)
        return mid;
    return 0;
}

int64_t sys_delete_module(const char *name_user, unsigned int flags)
{
    int priv = module_requires_priv();
    if (priv < 0)
        return priv;
    if (flags & ~0x1) /* MODULE_DELETE_FORCE is not supported */
        return -EINVAL;
    if (!name_user)
        return -EFAULT;

    char name[MODULE_NAME_MAX];
    if (user_strncpy(name, name_user, sizeof(name)) < 0)
        return -EFAULT;

    /* drvmod_unload() takes a module id; look it up by name through the
     * loader's public surface.  Since the module table is internal to the
     * loader, expose the lookup here via a small helper. */
    return drvmod_unload_by_name(name);
}
