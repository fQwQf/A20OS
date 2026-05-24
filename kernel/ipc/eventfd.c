#include "ipc/eventfd.h"

#include "core/consts.h"
#include "core/lock.h"
#include "core/sync.h"
#include "core/string.h"
#include "fs/anonfd.h"
#include "fs/file.h"
#include "fs/vfs.h"
#include "mm/slab.h"
#include "proc/proc.h"

typedef struct {
    spinlock_t    lock;
    wait_queue_t  readers;
    wait_queue_t  writers;
    uint64_t      counter;
    int           semaphore;
    int           nonblock;
} eventfd_t;

static int eventfd_read(vfile_t *vf, char *buf, size_t count)
{
    eventfd_t *efd = vf ? vf->priv : NULL;
    if (!efd) return -EBADF;
    if (count < sizeof(uint64_t)) return -EINVAL;

    spin_lock(&efd->lock);
    while (efd->counter == 0) {
        if (efd->nonblock) {
            spin_unlock(&efd->lock);
            return -EAGAIN;
        }
        spin_unlock(&efd->lock);
        wait_queue_sleep(&efd->readers);
        spin_lock(&efd->lock);
    }
    uint64_t val = efd->semaphore ? 1 : efd->counter;
    efd->counter -= val;
    int wake_writers = (efd->counter + val == ~0ULL);
    spin_unlock(&efd->lock);

    if (wake_writers)
        wait_queue_wake_all(&efd->writers);
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

    spin_lock(&efd->lock);
    while (efd->counter + val > ~0ULL - 1) {
        if (efd->nonblock) {
            spin_unlock(&efd->lock);
            return -EAGAIN;
        }
        spin_unlock(&efd->lock);
        wait_queue_sleep(&efd->writers);
        spin_lock(&efd->lock);
    }
    int wake_readers = (efd->counter == 0);
    efd->counter += val;
    spin_unlock(&efd->lock);

    if (wake_readers)
        wait_queue_wake_all(&efd->readers);
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
    spin_init(&efd->lock);
    wait_queue_init(&efd->readers);
    wait_queue_init(&efd->writers);
    efd->counter = initval;
    efd->semaphore = (flags & 1) != 0;
    efd->nonblock = (flags & O_NONBLOCK) != 0;
    vf->flags = O_RDWR | (flags & O_NONBLOCK);
    refcount_set(&vf->ref_count, 1);
    vf->ops = &g_eventfd_ops;
    vf->priv = efd;
    return anonfd_install_vfile(vf, flags);
}
