#include "ipc/eventfd.h"

#include "core/consts.h"
#include "core/string.h"
#include "fs/anonfd.h"
#include "fs/file.h"
#include "fs/vfs.h"
#include "mm/slab.h"
#include "proc/proc.h"

typedef struct {
    uint64_t counter;
    int semaphore;
    int nonblock;
} eventfd_t;

static int eventfd_read(vfile_t *vf, char *buf, size_t count)
{
    eventfd_t *efd = vf ? vf->priv : NULL;
    if (!efd) return -EBADF;
    if (count < sizeof(uint64_t)) return -EINVAL;
    while (efd->counter == 0) {
        if (efd->nonblock) return -EAGAIN;
        proc_yield();
    }
    uint64_t val = efd->semaphore ? 1 : efd->counter;
    efd->counter -= val;
    memcpy(buf, &val, sizeof(val));
    return sizeof(val);
}

static int eventfd_write(vfile_t *vf, const char *buf, size_t count)
{
    eventfd_t *efd = vf ? vf->priv : NULL;
    if (!efd) return -EBADF;
    if (count < sizeof(uint64_t)) return -EINVAL;
    uint64_t val;
    memcpy(&val, buf, sizeof(val));
    if (val == ~0ULL) return -EINVAL;
    efd->counter += val;
    return sizeof(val);
}

static vfile_ops_t g_eventfd_ops = {
    .read = eventfd_read,
    .write = eventfd_write,
    .close = anonfd_free_priv_close,
};

int eventfd_create(unsigned initval, int flags)
{
    if (flags & ~(O_CLOEXEC | O_NONBLOCK | 1)) return -EINVAL;
    eventfd_t *efd = kmalloc(sizeof(*efd));
    vfile_t *vf = vfile_alloc();
    if (!efd || !vf) {
        if (efd) kfree(efd);
        if (vf) vfile_free(vf);
        return -ENOMEM;
    }
    memset(efd, 0, sizeof(*efd));
    efd->counter = initval;
    efd->semaphore = (flags & 1) != 0;
    efd->nonblock = (flags & O_NONBLOCK) != 0;
    vf->flags = O_RDWR | (flags & O_NONBLOCK);
    refcount_set(&vf->ref_count, 1);
    vf->ops = &g_eventfd_ops;
    vf->priv = efd;
    return anonfd_install_vfile(vf, flags);
}
