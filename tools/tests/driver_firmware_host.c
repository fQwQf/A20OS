/* Production firmware core and VFS adapter; only allocator/filesystem mocked. */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "drivers/core/firmware.h"
#include "core/errno.h"
#include "core/fcntl.h"
#include "fs/vfs.h"

static unsigned allocations;
static int fail_alloc;
void *kmalloc(size_t size)
{
    if (fail_alloc) { fail_alloc = 0; return NULL; }
    void *p = malloc(size); assert(p); allocations++; return p;
}
void kfree(void *p) { if (p) { assert(allocations); allocations--; free(p); } }

static unsigned char payload[70001];
struct provider {
    uint64_t advertised;
    size_t actual, short_read;
    int open_error, read_error, oversized;
    unsigned opens, closes, releases, reads;
};
static int open_blob(void *context, const char *name, void **stream, uint64_t *size)
{
    struct provider *p = context;
    assert(!strcmp(name, "vendor/gpu.bin"));
    p->opens++;
    if (p->open_error) return p->open_error;
    *stream = p; *size = p->advertised; return 0;
}
static int read_blob(void *context, void *stream, uint64_t offset,
                       void *buffer, size_t count)
{
    struct provider *p = context;
    assert(p == stream && count && count <= 65536);
    p->reads++;
    if (p->read_error) return p->read_error;
    if (p->oversized) return (int)count + 1;
    if (offset >= p->actual) return 0;
    if (count > p->actual - offset) count = p->actual - offset;
    if (p->short_read && count > p->short_read) count = p->short_read;
    memcpy(buffer, payload + offset, count);
    return (int)count;
}
static void close_blob(void *context, void *stream)
{ struct provider *p = context; assert(p == stream); p->closes++; }
static void release_blob(void *context) { ((struct provider *)context)->releases++; }

static void provider_tests(void)
{
    struct provider p = {.advertised=sizeof(payload), .actual=sizeof(payload)};
    struct driver_firmware_ops ops = {open_blob, read_blob, close_blob, release_blob};
    struct driver_firmware_source *source = (void *)1;
    struct driver_firmware *image = (void *)1;
    assert(driver_firmware_source_create(NULL, &p, &source) == -EINVAL && !source);
    fail_alloc = 1;
    assert(driver_firmware_source_create(&ops, &p, &source) == -ENOMEM && !source);
    assert(!p.releases);
    assert(!driver_firmware_source_create(&ops, &p, &source));
    memset(&ops, 0, sizeof(ops)); /* Source must copy the caller's ops. */
    const char *bad[] = {NULL, "", "/x", "../x", "x/..", "x/./y", "x//y", "x/"};
    for (size_t i = 0; i < sizeof(bad)/sizeof(*bad); i++) {
        image = (void *)1;
        assert(driver_firmware_load(source, bad[i], sizeof(payload), &image) == -EINVAL);
        assert(!image && !p.opens);
    }
    char long_name[DRIVER_FIRMWARE_NAME_MAX + 2];
    memset(long_name, 'a', sizeof(long_name)); long_name[sizeof(long_name)-1] = 0;
    assert(driver_firmware_load(source, long_name, sizeof(payload), &image) == -ENAMETOOLONG);
    assert(driver_firmware_load(source, "vendor/gpu.bin", 0, &image) == -EINVAL);
    assert(!p.opens);
    assert(!driver_firmware_load(source, "vendor/gpu.bin", sizeof(payload), &image));
    assert(p.reads == 3 && p.opens == p.closes);
    assert(driver_firmware_size(image) == sizeof(payload));
    assert(!memcmp(driver_firmware_data(image), payload, sizeof(payload)));
    driver_firmware_get(image); driver_firmware_put(image); driver_firmware_put(image);

    p.short_read = 13;
    assert(!driver_firmware_load(source, "vendor/gpu.bin", sizeof(payload), &image));
    assert(!memcmp(driver_firmware_data(image), payload, sizeof(payload)));
    driver_firmware_put(image); p.short_read = 0;
    fail_alloc = 1;
    assert(driver_firmware_load(source, "vendor/gpu.bin", sizeof(payload), &image) == -ENOMEM);
    assert(!image && p.opens == p.closes);
    p.read_error = -EINTR;
    assert(driver_firmware_load(source, "vendor/gpu.bin", sizeof(payload), &image) == -EINTR);
    p.read_error = 0; p.actual--;
    assert(driver_firmware_load(source, "vendor/gpu.bin", sizeof(payload), &image) == -EIO);
    p.actual++; p.advertised--;
    assert(driver_firmware_load(source, "vendor/gpu.bin", sizeof(payload), &image) == -EIO);
    p.advertised++; p.oversized = 1;
    assert(driver_firmware_load(source, "vendor/gpu.bin", sizeof(payload), &image) == -EIO);
    p.oversized = 0;
    assert(driver_firmware_load(source, "vendor/gpu.bin", sizeof(payload)-1, &image) == -EFBIG);
    p.advertised = ~(uint64_t)0;
    assert(driver_firmware_load(source, "vendor/gpu.bin", SIZE_MAX, &image) == -EFBIG);
    p.advertised = 0;
    assert(driver_firmware_load(source, "vendor/gpu.bin", sizeof(payload), &image) == -ENODATA);
    assert(!image && p.opens == p.closes && allocations == 1);
    p.open_error = -ENOENT;
    unsigned closes = p.closes;
    assert(driver_firmware_load(source, "vendor/gpu.bin", sizeof(payload), &image) == -ENOENT);
    assert(!image && closes == p.closes);
    p.open_error = 1;
    assert(driver_firmware_load(source, "vendor/gpu.bin", sizeof(payload), &image) == -EIO);
    p.open_error = 0; p.advertised = sizeof(payload);
    assert(!driver_firmware_load(source, "vendor/gpu.bin", sizeof(payload), &image));
    driver_firmware_source_get(source);
    driver_firmware_source_put(source); assert(!p.releases);
    driver_firmware_source_put(source); assert(p.releases == 1);
    assert(!memcmp(driver_firmware_data(image), payload, sizeof(payload)));
    driver_firmware_put(image); assert(!allocations);

    unsigned char original[] = {1, 2, 3};
    assert(!driver_firmware_from_memory(original, sizeof(original), 3, &image));
    original[0] = 9;
    assert(((const unsigned char *)driver_firmware_data(image))[0] == 1);
    driver_firmware_put(image);
    assert(driver_firmware_from_memory(original, 0, 3, &image) == -ENODATA);
    assert(driver_firmware_from_memory(original, 3, 2, &image) == -EFBIG);
    assert(!image && !allocations);
}

static mount_t mount, other_mount;
static vnode_t root, vendor, blob;
static vfile_t directory;
static unsigned admission, mount_pins, file_opens, file_closes;
static int shutdown_active, stat_error, open_fail, read_error;
int mntns_fileop_enter(void)
{ if (shutdown_active) return -ESHUTDOWN; admission++; return 0; }
void mntns_fileop_leave(void) { assert(admission); admission--; }
void vnode_get(vnode_t *vn) { assert(refcount_read(&vn->ref_count)); refcount_inc(&vn->ref_count); }
void vnode_put(vnode_t *vn) { assert(refcount_read(&vn->ref_count) > 1); refcount_dec_and_test(&vn->ref_count); }
void vfile_get(vfile_t *vf) { refcount_inc(&vf->ref_count); }
void vfs_mount_vfile_get(mount_t *m) { assert(admission && m == &mount); mount_pins++; }
void vfs_put_file(vfile_t *vf)
{
    if (!refcount_dec_and_test(&vf->ref_count)) return;
    if (vf->mount_open_counted) { assert(mount_pins); mount_pins--; }
    vnode_put(vf->vnode);
    if (vf != &directory) { file_closes++; kfree(vf); }
}
static int lookup(vnode_t *vn, const char *name, vnode_t **out)
{
    assert(admission);
    if (vn == &root && !strcmp(name, "vendor")) *out = &vendor;
    else if (vn == &vendor && !strcmp(name, "gpu.bin")) *out = &blob;
    else return -ENOENT;
    vnode_get(*out); return 0;
}
static vfile_t *open_file(vnode_t *vn, int flags)
{
    assert(admission && vn == &blob && flags == O_RDONLY);
    if (open_fail) return NULL;
    vfile_t *file = kmalloc(sizeof(*file)); assert(file);
    memset(file, 0, sizeof(*file)); file->vnode = vn;
    vnode_get(vn); refcount_set(&file->ref_count, 1); file_opens++; return file;
}
int vfs_vfile_stat(vfile_t *vf, kstat_t *st)
{
    assert(admission && vf->vnode == &blob);
    if (stat_error) return stat_error;
    st->st_size = sizeof(payload); return 0;
}
int vfs_pread_file(vfile_t *vf, char *buffer, size_t count, uint64_t offset)
{
    assert(admission && vf->vnode == &blob);
    if (read_error) return read_error;
    if (offset >= sizeof(payload)) return 0;
    if (count > sizeof(payload)-offset) count = sizeof(payload)-offset;
    memcpy(buffer, payload + offset, count); return (int)count;
}
static void vfs_tests(void)
{
    vnode_ops_t ops = {.lookup=lookup, .open=open_file};
    root = (vnode_t){.type=VFS_FT_DIR, .mnt=&mount, .ops=&ops};
    vendor = root;
    blob = (vnode_t){.type=VFS_FT_REGULAR, .mode=S_IFREG|0644, .mnt=&mount, .ops=&ops};
    refcount_set(&root.ref_count, 2); /* fixture + open directory */
    refcount_set(&vendor.ref_count, 1); refcount_set(&blob.ref_count, 1);
    directory = (vfile_t){.vnode=&root, .mount_open_counted=1};
    refcount_set(&directory.ref_count, 1); mount_pins = 1;
    struct driver_firmware_source *source;
    struct driver_firmware *image;
    fail_alloc = 1;
    assert(driver_firmware_source_create_vfs(&directory, &source) == -ENOMEM);
    assert(refcount_read(&directory.ref_count) == 1 && !admission);
    assert(!driver_firmware_source_create_vfs(&directory, &source));
    vfs_put_file(&directory); /* Caller closes its FD; the source still works. */
    assert(!driver_firmware_load(source, "vendor/gpu.bin", sizeof(payload), &image));
    assert(!admission && mount_pins == 1 && file_opens == file_closes);
    assert(!memcmp(driver_firmware_data(image), payload, sizeof(payload)));
    driver_firmware_put(image);
    assert(driver_firmware_load(source, "missing", sizeof(payload), &image) == -ENOENT);
    assert(driver_firmware_load(source, "vendor", sizeof(payload), &image) == -EINVAL);
    assert(driver_firmware_load(source, "vendor/gpu.bin/x", sizeof(payload), &image) == -ENOTDIR);
    vendor.type = VFS_FT_SYMLINK;
    assert(driver_firmware_load(source, "vendor/gpu.bin", sizeof(payload), &image) == -ELOOP);
    vendor.type = VFS_FT_DIR; blob.type = VFS_FT_SYMLINK;
    assert(driver_firmware_load(source, "vendor/gpu.bin", sizeof(payload), &image) == -ELOOP);
    blob.type = VFS_FT_REGULAR; blob.mode = S_IFIFO|0644;
    unsigned opens = file_opens;
    assert(driver_firmware_load(source, "vendor/gpu.bin", sizeof(payload), &image) == -EINVAL);
    assert(opens == file_opens); /* FIFO rejected before backend open. */
    blob.mode = S_IFREG|0644; blob.mnt = &other_mount;
    assert(driver_firmware_load(source, "vendor/gpu.bin", sizeof(payload), &image) == -EXDEV);
    blob.mnt = &mount; stat_error = -EIO;
    assert(driver_firmware_load(source, "vendor/gpu.bin", sizeof(payload), &image) == -EIO);
    stat_error = 0; open_fail = 1;
    assert(driver_firmware_load(source, "vendor/gpu.bin", sizeof(payload), &image) == -ENOMEM);
    open_fail = 0; read_error = -EIO;
    assert(driver_firmware_load(source, "vendor/gpu.bin", sizeof(payload), &image) == -EIO);
    read_error = 0; shutdown_active = 1;
    assert(driver_firmware_load(source, "vendor/gpu.bin", sizeof(payload), &image) == -ESHUTDOWN);
    shutdown_active = 0; directory.backend_close_done = 1;
    assert(driver_firmware_load(source, "vendor/gpu.bin", sizeof(payload), &image) == -ESHUTDOWN);
    struct driver_firmware_source *failed = (void *)1;
    assert(driver_firmware_source_create_vfs(&directory, &failed) == -ESHUTDOWN);
    assert(!failed && !image && !admission && mount_pins == 1);
    driver_firmware_source_put(source);
    assert(!mount_pins && !allocations && file_opens == file_closes);
    assert(refcount_read(&root.ref_count) == 1 &&
           refcount_read(&vendor.ref_count) == 1 && refcount_read(&blob.ref_count) == 1);
}

int main(void)
{
    for (size_t i = 0; i < sizeof(payload); i++) payload[i] = (unsigned char)(i*17);
    provider_tests();
    vfs_tests();
    puts("driver firmware: copies, short reads, rollback and VFS anchoring PASS");
    return 0;
}
