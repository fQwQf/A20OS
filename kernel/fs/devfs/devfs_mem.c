/*
 * devfs memory-style character devices: /dev/null, /dev/zero, /dev/random,
 * /dev/urandom, /dev/full, /dev/kmsg, /dev/cpu_dma_latency.
 *
 * Split out of devfs.c to keep the node-table/lookup core small.  The ops
 * tables are referenced by devfs.c through devfs_internal.h.
 */

#include "devfs_internal.h"

#include "core/klog.h"
#include "core/random.h"
#include "core/string.h"
#include "core/sync.h"

/* ---- shared helpers ---- */

int devfs_null_read(vfile_t *vf, char *buf, size_t count) {
    (void)vf; (void)buf; (void)count;
    return 0;
}

int devfs_null_write(vfile_t *vf, const char *buf, size_t count) {
    (void)vf; (void)buf;
    return (int)count;
}

static int devfs_zero_read(vfile_t *vf, char *buf, size_t count) {
    (void)vf;
    memset(buf, 0, count);
    return (int)count;
}

static int devfs_random_read(vfile_t *vf, char *buf, size_t count) {
    (void)vf;
    random_fill(buf, count);
    return (int)count;
}

vfile_ops_t g_devfs_null_ops =
    { .read = devfs_null_read, .write = devfs_null_write,
      .lseek = devfs_noop_lseek };
vfile_ops_t g_devfs_zero_ops =
    { .read = devfs_zero_read, .write = devfs_null_write,
      .lseek = devfs_noop_lseek };
vfile_ops_t g_devfs_random_ops =
    { .read = devfs_random_read, .write = devfs_null_write,
      .lseek = devfs_noop_lseek };

/* ---- /dev/full ---- */

static int devfs_full_read(vfile_t *vf, char *buf, size_t count)
{
    (void)vf;
    memset(buf, 0, count);
    return (int)count;
}

static int devfs_full_write(vfile_t *vf, const char *buf, size_t count)
{
    (void)vf;
    (void)buf;
    (void)count;
    return -ENOSPC;
}

vfile_ops_t g_devfs_full_ops =
    { .read = devfs_full_read, .write = devfs_full_write,
      .lseek = devfs_noop_lseek };

/* ---- /dev/kmsg ---- */

static size_t g_kmsg_pos;

static int devfs_kmsg_read(vfile_t *vf, char *buf, size_t count)
{
    (void)vf;
    size_t consumed = 0;
    size_t cur = g_kmsg_pos;
    int n;
    while (consumed < count && (n = klog_read(buf + consumed,
                                              count - consumed,
                                              &cur)) > 0) {
        consumed += (size_t)n;
    }
    g_kmsg_pos = cur;
    return (int)consumed;
}

static int devfs_kmsg_write(vfile_t *vf, const char *buf, size_t count)
{
    (void)vf;
    klog_write_raw(buf, count);
    return (int)count;
}

vfile_ops_t g_devfs_kmsg_ops =
    { .read = devfs_kmsg_read, .write = devfs_kmsg_write,
      .lseek = devfs_noop_lseek };
