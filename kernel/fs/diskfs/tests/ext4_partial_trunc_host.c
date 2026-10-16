/*
 * ext4_partial_trunc_host.c — host end-to-end harness for the shared ext4
 * partial-truncate path (kernel/fs/diskfs/ext4*.c compiled unmodified through
 * the fscompat environment, exactly as the native UFSD backend links them).
 *
 * The harness owns only host glue: an fsio_* implementation over a plain
 * image file, the perf globals the inline counters reference, and a small
 * scenario driver.  Every filesystem decision (mount, lookup, create,
 * writepage, truncate, reclaim) executes the real shared sources.
 *
 * Scenarios (argv after the image path):
 *   statfs                 print "STATFS free=<blocks>"
 *   mkfile:<name>:<pg,...> create /name, extend to cover the pages, and
 *                          write one 4 KiB pattern page per listed page
 *   trunc:<name>:<size>    vnode truncate /name to <size>; print rc
 *   trunc-retry:<name>:<size>:<oldsize>  expect one injected EIO, verify the
 *                          old size is visible, then retry successfully
 *
 * Build with -DCONFIG_EXT4_RUST_EXTENT_TXN to route extent-tree partial
 * truncates through the Rust transaction (link ext4_extent_txn.o), or
 * without it to run the legacy C reference path for C/Rust comparison.
 *
 * No libc headers are included: glibc's struct-stat macros (st_atime, ...)
 * would corrupt kernel/fs/vfs.h's kstat layout, and fscompat/compat.c
 * supplies printf/memory primitives at link time.  The handful of syscalls
 * below use the stable cdecl ABI directly.
 */
#include <stdint.h>
#include <stddef.h>

/* fscompat redirects malloc through liba20rt in production. For this
 * glibc-only harness, bypass that wrapper to the host allocator. */
extern void *__libc_malloc(unsigned long size);
extern void __libc_free(void *ptr);
extern void *__libc_realloc(void *ptr, unsigned long size);
void *a20_malloc(uint64_t size) { return __libc_malloc((unsigned long)size); }
void a20_free(void *ptr) { __libc_free(ptr); }
void *a20_realloc(void *ptr, uint64_t size)
{ return __libc_realloc(ptr, (unsigned long)size); }

extern int open(const char *path, int flags, ...);
extern int close(int fd);
extern long pread(int fd, void *buf, unsigned long count, long offset);
extern long pwrite(int fd, const void *buf, unsigned long count, long offset);
extern long lseek(int fd, long offset, int whence);
extern int fsync(int fd);
extern long write(int fd, const void *buf, unsigned long count);

#define HOST_O_RDWR 2

#include "core/string.h"
#include "core/stdio.h"
#include "fs/ext4.h"
#include "fs/ext4_internal.h"
#include "fs/vfs.h"
#include "fs/block_cache.h"
#include "core/perf.h"

extern int ext4_vn_readpage(vnode_t *vn, uint64_t index, void *data, size_t len);

/* fsio_* — the device I/O contract ufs_vnfs_backend.c expects. */
static int g_fd = -1;

int fsio_read(uint32_t lba, void *buf, uint32_t count)
{
    if (g_fd < 0 || !buf) return -1;
    long off = (long)lba * 512;
    long n = pread(g_fd, buf, (unsigned long)count * 512, off);
    return n == (long)count * 512 ? 0 : -1;
}

int fsio_write(uint32_t lba, const void *buf, uint32_t count)
{
    if (g_fd < 0 || !buf) return -1;
    long off = (long)lba * 512;
    long n = pwrite(g_fd, buf, (unsigned long)count * 512, off);
    return n == (long)count * 512 ? 0 : -1;
}

uint64_t fsio_capacity_sectors(void)
{
    if (g_fd < 0) return 0;
    long end = lseek(g_fd, 0, 2 /* SEEK_END */);
    return end < 0 ? 0 : (uint64_t)end / 512;
}

uint64_t fsio_monotonic_ns(void)
{
    static uint64_t tick;
    return ++tick;
}

int fsio_flush_supported(void) { return 1; }

int fsio_flush(void)
{
    return g_fd >= 0 && fsync(g_fd) == 0 ? 0 : -1;
}

/* block_dev_t provider hooks (ufs_vnfs_backend.c contract). */
static int dev_read_sector(struct block_dev *dev, uint64_t lba,
                           void *buf, size_t count)
{
    (void)dev;
    return fsio_read((uint32_t)lba, buf, (uint32_t)count);
}

static int dev_write_sector(struct block_dev *dev, uint64_t lba,
                            const void *buf, size_t count)
{
    (void)dev;
    return fsio_write((uint32_t)lba, buf, (uint32_t)count);
}

static int dev_flush(struct block_dev *dev)
{
    (void)dev;
    return fsio_flush();
}

static bcache_t *g_bc;
static vnode_t *g_root;
static int g_scenario_failed;
static uint64_t g_initial_free;
static unsigned g_statfs_calls;
static uint64_t g_expected_reclaim_delta = 2;
extern void (*fscompat_log_sink)(const char *line);

static void host_report(const char *message)
{
    size_t n = 0;
    while (message[n]) n++;
    (void)write(2, message, (unsigned long)n);
}

static void host_log_sink(const char *line)
{
    host_report(line);
    host_report("\n");
}

static int mount_image(const char *path)
{
    g_fd = open(path, HOST_O_RDWR);
    if (g_fd < 0) {
        printf("open %s failed\n", path);
        host_report("HOST_OPEN_FAIL\n");
        return -1;
    }
    static block_dev_t dev;
    dev.read_sector = dev_read_sector;
    dev.write_sector = dev_write_sector;
    dev.flush = fsio_flush_supported() ? dev_flush : NULL;
    dev.sector_size = 512;
    dev.capacity = fsio_capacity_sectors();
    dev.priv = 0;
    g_bc = bcache_create(&dev);
    if (!g_bc) {
        printf("bcache_create failed\n");
        host_report("HOST_BCACHE_FAIL\n");
        return -1;
    }
    g_root = ext4_mount(g_bc);
    if (!g_root) {
        printf("ext4_mount failed\n");
        host_report("HOST_EXT4_MOUNT_FAIL\n");
        return -1;
    }
    host_report("HOST_MOUNT_PASS\n");
    return 0;
}

static void unmount_image(void)
{
    if (g_root) ext4_unmount(g_root);
    g_root = NULL;
    if (g_bc) bcache_destroy(g_bc);
    g_bc = NULL;
    if (g_fd >= 0) { close(g_fd); g_fd = -1; }
}

static void do_statfs(void)
{
    kstatfs_t st;
    memset(&st, 0, sizeof(st));
    int rc = ext4_vn_statfs(g_root, &st);
    if (rc < 0) {
        host_report("HOST_STATFS_FAIL\n");
        g_scenario_failed = 1;
        return;
    }
    if (g_statfs_calls == 0) {
        g_initial_free = st.f_bfree;
        host_report("HOST_STATFS_BASE_CAPTURED\n");
    } else if (g_statfs_calls == 1) {
        if (st.f_bfree != g_initial_free + g_expected_reclaim_delta) {
            host_report("HOST_EXPECTED_RECLAIM_FAIL delta_mismatch\n");
            g_scenario_failed = 1;
        } else if (g_expected_reclaim_delta == 2) {
            host_report("HOST_EXPECTED_RECLAIM_PASS delta=2\n");
        } else if (g_expected_reclaim_delta == 5) {
            host_report("HOST_EXPECTED_RECLAIM_PASS delta=5\n");
        } else {
            host_report("HOST_EXPECTED_RECLAIM_FAIL unsupported_delta\n");
            g_scenario_failed = 1;
        }
    }
    g_statfs_calls++;
    printf("STATFS free=%llu blocks=%llu\n",
           (unsigned long long)st.f_bfree, (unsigned long long)st.f_blocks);
}

static uint32_t parse_page(char *tok, int *ok)
{
    uint32_t v = 0;
    *ok = 0;
    if (!tok || !*tok) return 0;
    for (; *tok; tok++) {
        if (*tok < '0' || *tok > '9') return 0;
        v = v * 10 + (uint32_t)(*tok - '0');
    }
    *ok = 1;
    return v;
}

/* Split a comma list in place, invoking cb for each element. */
static int for_each_page(char *pages, uint32_t *out, int max)
{
    int n = 0;
    char *p = pages;
    while (p && n < max) {
        char *comma = strchr(p, ',');
        if (comma) *comma = '\0';
        int ok;
        uint32_t pg = parse_page(p, &ok);
        if (!ok) break;
        out[n++] = pg;
        p = comma ? comma + 1 : NULL;
    }
    return n;
}

static void do_mkfile(const char *name, char *pages)
{
    enum { MAX_PAGES = 64 };
    uint32_t pglist[MAX_PAGES];
    int npg = for_each_page(pages, pglist, MAX_PAGES);
    if (npg == 0) { printf("MKFILE %s bad_pages\n", name); return; }

    vnode_t *vn = NULL;
    int rc = ext4_vn_create(g_root, name, 0644, &vn);
    if (rc < 0) { printf("MKFILE %s create_rc=%d\n", name, rc); g_scenario_failed = 1; return; }

    uint32_t max_page = 0;
    for (int i = 0; i < npg; i++)
        if (pglist[i] > max_page) max_page = pglist[i];
    uint64_t want = ((uint64_t)max_page + 1) * 4096;
    rc = ext4_vn_truncate(vn, (size_t)want);
    if (rc < 0) {
        printf("MKFILE %s extend_rc=%d\n", name, rc);
        vnode_put(vn);
        g_scenario_failed = 1;
        return;
    }

    for (int i = 0; i < npg; i++) {
        static char page[4096];
        uint32_t pg = pglist[i];
        memset(page, (int)(0x41 + (pg % 26)), sizeof(page));
        memcpy(page, &pg, sizeof(pg));
        int wr = ext4_vn_writepage(vn, pg, page, sizeof(page));
        if (wr < 0) {
            printf("MKFILE %s page=%u write_rc=%d\n", name, pg, wr);
            g_scenario_failed = 1;
        }
    }
    if (strcmp(name, "depth1") == 0) {
        ext4_vnode_priv_t *priv = (ext4_vnode_priv_t *)vn->fs_data;
        ext4_inode_t inode;
        if (ext4_read_inode(priv->sb, priv->inode_num, &inode) < 0 ||
            !(inode.i_flags & EXT4_EXTENTS_FL) ||
            ((ext4_extent_header_t *)((uint8_t *)&inode +
              offsetof(ext4_inode_t, i_block)))->eh_depth != 1) {
            host_report("HOST_DEPTH1_PRECONDITION_FAIL\n");
            g_scenario_failed = 1;
        } else {
            host_report("HOST_DEPTH1_PRECONDITION_PASS\n");
        }
    }
    vnode_put(vn);
    if (!g_scenario_failed) host_report("HOST_MKFILE_PASS\n");
    printf("MKFILE %s ok\n", name);
}

static uint64_t parse_u64(const char *s, int *ok)
{
    uint64_t v = 0;
    *ok = 0;
    if (!s || !*s) return 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return 0;
        v = v * 10 + (uint64_t)(*s - '0');
    }
    *ok = 1;
    return v;
}

static int verify_single_pattern(vnode_t *vn, uint64_t size)
{
    static uint8_t page[4096];
    for (uint64_t off = 0; off < size; off += sizeof(page)) {
        size_t want = (size_t)(size - off);
        if (want > sizeof(page)) want = sizeof(page);
        int got = ext4_vn_readpage(vn, off / sizeof(page), page, want);
        if (got != (int)want) return 0;
        for (size_t i = 0; i < want; i++)
            if (page[i] != (uint8_t)((off + i) % 251 + 1)) return 0;
    }
    return 1;
}

static void do_trunc(const char *name, uint64_t size)
{
    vnode_t *vn = NULL;
    int rc = ext4_lookup(g_root, name, &vn);
    if (rc < 0) {
        printf("TRUNC %s lookup_rc=%d\n", name, rc);
        g_scenario_failed = 1;
        return;
    }
    rc = ext4_vn_truncate(vn, (size_t)size);
    printf("TRUNC %s size=%llu rc=%d\n", name,
           (unsigned long long)size, rc);
    if (rc < 0 || vn->size != (size_t)size) {
        host_report("HOST_TRUNC_FAIL rc_or_size\n");
        g_scenario_failed = 1;
    } else {
        host_report("HOST_TRUNC_PASS\n");
        if (strcmp(name, "single") == 0 && size >= 4096) {
            int valid = verify_single_pattern(vn, size);
            if (valid) host_report("HOST_CONTENT_PASS\n");
            else {
                host_report("HOST_CONTENT_FAIL\n");
                g_scenario_failed = 1;
            }
        }
    }
    vnode_put(vn);
}

static int do_trunc_retry(const char *name, uint64_t size, uint64_t old_size)
{
    vnode_t *vn = NULL;
    int rc = ext4_lookup(g_root, name, &vn);
    if (rc < 0) return rc;
    if (vn->size != old_size) {
        printf("TRUNC_RETRY initial_size=%llu expected=%llu\n",
               (unsigned long long)vn->size, (unsigned long long)old_size);
        vnode_put(vn);
        return -1;
    }
    rc = ext4_vn_truncate(vn, (size_t)size);
    if (rc != -5 || vn->size != old_size ||
        !verify_single_pattern(vn, old_size)) {
        printf("TRUNC_RETRY first_rc=%d size=%llu expected_eio=-5 old=%llu\n",
               rc, (unsigned long long)vn->size, (unsigned long long)old_size);
        vnode_put(vn);
        return -1;
    }
    printf("TRUNC_RETRY injected_eio old_size=%llu\n",
           (unsigned long long)vn->size);
    host_report("HOST_RETRY_OLD_CONTENT_PASS\n");
    rc = ext4_vn_truncate(vn, (size_t)size);
    printf("TRUNC_RETRY second_rc=%d size=%llu\n", rc,
           (unsigned long long)vn->size);
    if (rc >= 0 && vn->size == (size_t)size)
        host_report("HOST_RETRY_PASS\n");
    else {
        host_report("HOST_RETRY_FAIL\n");
        g_scenario_failed = 1;
    }
    vnode_put(vn);
    return rc;
}

int main(int argc, char **argv)
{
    fscompat_log_sink = host_log_sink;
    host_report("HOST_START\n");
    if (argc < 3) {
        printf("usage: %s <image> <scenario...>\n", argv[0]);
        return 64;
    }
    if (mount_image(argv[1]) < 0)
        return 1;
    for (int i = 2; i < argc; i++) {
        char *spec = argv[i];
        if (strcmp(spec, "statfs") == 0) {
            do_statfs();
        } else if (strncmp(spec, "expectdelta:", 12) == 0) {
            int ok;
            uint64_t delta = parse_u64(spec + 12, &ok);
            if (!ok || (delta != 2 && delta != 5)) {
                host_report("HOST_EXPECTDELTA_FAIL\n");
                g_scenario_failed = 1;
                return 64;
            }
            g_expected_reclaim_delta = delta;
        } else if (strncmp(spec, "mkfile:", 7) == 0) {
            char *name = spec + 7;
            char *pages = strchr(name, ':');
            if (!pages) { printf("bad mkfile spec\n"); return 64; }
            *pages++ = '\0';
            do_mkfile(name, pages);
        } else if (strncmp(spec, "trunc:", 6) == 0) {
            char *name = spec + 6;
            char *size = strchr(name, ':');
            if (!size) { printf("bad trunc spec\n"); return 64; }
            *size++ = '\0';
            int ok;
            uint64_t v = parse_u64(size, &ok);
            if (!ok) { printf("bad trunc size\n"); return 64; }
            do_trunc(name, v);
        } else if (strncmp(spec, "trunc-retry:", 12) == 0) {
            char *name = spec + 12;
            char *size = strchr(name, ':');
            if (!size) { printf("bad trunc-retry spec\n"); return 64; }
            *size++ = '\0';
            char *old = strchr(size, ':');
            if (!old) { printf("bad trunc-retry old size\n"); return 64; }
            *old++ = '\0';
            int ok_size, ok_old;
            uint64_t v = parse_u64(size, &ok_size);
            uint64_t o = parse_u64(old, &ok_old);
            if (!ok_size || !ok_old) { printf("bad retry sizes\n"); return 64; }
            if (do_trunc_retry(name, v, o) < 0) return 1;
        } else {
            printf("unknown scenario: %s\n", spec);
            return 64;
        }
    }
    unmount_image();
    if (g_scenario_failed) return 1;
    host_report("HOST_SCENARIOS_PASS\n");
    return 0;
}
