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
static vnode_t *g_extend_during_write;

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
    /* Deterministically interleave buffered EOF growth with an older
     * filesystem writeback, without relying on host scheduling. */
    if (g_extend_during_write) {
        g_extend_during_write->size = 8192;
        ((ext4_vnode_priv_t *)g_extend_during_write->fs_data)->file_size = 8192;
        g_extend_during_write = NULL;
    }
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

static void do_writeback_size_race(void)
{
    vnode_t *vn = NULL;
    static char page[4096];
    memset(page, 0x73, sizeof(page));
    if (ext4_vn_create(g_root, "eof-race", 0600, &vn) < 0 ||
        ext4_vn_truncate(vn, 4096) < 0) {
        g_scenario_failed = 1;
        return;
    }
    g_extend_during_write = vn;
    int ret = ext4_vn_writepage(vn, 0, page, sizeof(page));
    if (ret < 0 || g_extend_during_write || vn->size != 8192 ||
        ((ext4_vnode_priv_t *)vn->fs_data)->file_size != 8192) {
        host_report("HOST_WRITEBACK_EOF_RACE_FAIL\n");
        g_scenario_failed = 1;
    } else {
        host_report("HOST_WRITEBACK_EOF_RACE_PASS\n");
    }
    g_extend_during_write = NULL;
    vnode_put(vn);
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

static void do_desktop_regression(void)
{
    char pages[] = "1,3,5";
    do_mkfile("desktop-sparse", pages);
    vnode_t *vn = NULL;
    if (g_scenario_failed || ext4_lookup(g_root, "desktop-sparse", &vn) < 0) {
        g_scenario_failed = 1;
        return;
    }
    ext4_vnode_priv_t *priv = vn->fs_data;
    ext4_inode_t before, after;
    uint64_t old_blocks[3];
    if (ext4_read_inode(priv->sb, priv->inode_num, &before) < 0) {
        g_scenario_failed = 1;
        vnode_put(vn);
        return;
    }
    for (int i = 0; i < 3; i++)
        old_blocks[i] = ext4_block_map(priv->sb, &before, 1 + i * 2);
    static char batch[6 * 4096], readback[sizeof(batch)];
    for (size_t i = 0; i < sizeof(batch); i++)
        batch[i] = (char)(1 + i % 251);
    int rc = ext4_vn_writepages(vn, 0, batch, sizeof(batch));
    if (rc < 0 || ext4_read_inode(priv->sb, priv->inode_num, &after) < 0) {
        g_scenario_failed = 1;
    } else {
        for (int i = 0; i < 3; i++) {
            if (!old_blocks[i] || ext4_block_map(priv->sb, &after, 1 + i * 2) != old_blocks[i]) {
                host_report("HOST_DESKTOP_BATCH_REPLACED_EXISTING_BLOCK\n");
                g_scenario_failed = 1;
            }
        }
        rc = ext4_vn_readpages(vn, 0, readback, sizeof(readback));
        if (rc != sizeof(readback) || memcmp(batch, readback, sizeof(batch))) {
            host_report("HOST_DESKTOP_BATCH_CONTENT_FAIL\n");
            g_scenario_failed = 1;
        }
    }
    vnode_put(vn);
    /* Cross a full leaf, then fill holes in both halves. The permutation
     * forces insertion before/between old extents and updates root keys. */
    vn = NULL;
    if (ext4_vn_create(g_root, "desktop-tree", 0644, &vn) < 0 ||
        ext4_vn_truncate(vn, 800 * 4096) < 0) {
        g_scenario_failed = 1;
        return;
    }
    static char tree_page[4096];
    for (unsigned i = 0; i < 400 && !g_scenario_failed; i++) {
        unsigned page = 2 * ((i * 167) % 400);
        memset(tree_page, 1 + page % 251, sizeof(tree_page));
        if (ext4_vn_writepage(vn, page, tree_page, sizeof(tree_page)) < 0)
            g_scenario_failed = 1;
    }
    for (unsigned page = 0; page < 800 && !g_scenario_failed; page++) {
        memset(tree_page, 0, sizeof(tree_page));
        if (ext4_vn_readpage(vn, page, tree_page, sizeof(tree_page)) < 0) {
            g_scenario_failed = 1;
            break;
        }
        for (unsigned j = 0; j < sizeof(tree_page); j++) {
            unsigned char expected = page % 2 ? 0 : 1 + page % 251;
            if ((unsigned char)tree_page[j] != expected)
                g_scenario_failed = 1;
        }
    }
    vnode_put(vn);
    if (g_scenario_failed) host_report("HOST_DESKTOP_TREE_FAIL\n");
    if (ext4_vn_symlink(g_root, "desktop-lock", "127.0.1.1:+189") < 0 ||
        ext4_vn_unlink(g_root, "desktop-lock") < 0) {
        host_report("HOST_DESKTOP_SYMLINK_UNLINK_FAIL\n");
        g_scenario_failed = 1;
    }
    if (!g_scenario_failed)
        host_report("HOST_DESKTOP_BATCH_AND_SYMLINK_PASS\n");
}

static void do_desktop_shutdown(void)
{
    char pages[] = "0";
    do_mkfile("held-temp", pages);
    vnode_t *held_file = NULL, *held_link = NULL;
    if (ext4_lookup(g_root, "held-temp", &held_file) < 0 ||
        ext4_vn_unlink(g_root, "held-temp") < 0 ||
        ext4_vn_symlink(g_root, "held-lock", "127.0.1.1:+189") < 0 ||
        ext4_lookup(g_root, "held-lock", &held_link) < 0 ||
        ext4_vn_unlink(g_root, "held-lock") < 0 ||
        ext4_shutdown_clean(g_root) < 0) {
        host_report("HOST_DESKTOP_SHUTDOWN_FAIL\n");
        g_scenario_failed = 1;
        return;
    }
    /* Model firmware poweroff: deliberately keep the cached/mapped vnode
     * references alive, with no more access after the final shutdown gate. */
    host_report("HOST_DESKTOP_SHUTDOWN_PASS\n");
}

static void do_indexed_lazy_regression(void)
{
    vnode_t *dir = NULL, *file = NULL, *old = NULL;
    if (ext4_lookup(g_root, "indexed", &dir) < 0) goto fail;
    ext4_vnode_priv_t *priv = dir->fs_data;
    ext4_sb_info_t *sb = priv->sb;
    ext4_inode_t inode;
    if (ext4_read_inode(sb, priv->inode_num, &inode) < 0 ||
        !(inode.i_flags & EXT4_INDEX_FL)) goto fail;
    if (ext4_vn_create(dir, "apk-added", 0644, &file) < 0) goto fail;
    vnode_put(file); file = NULL;
    if (ext4_vn_unlink(dir, "old-000001") < 0 ||
        ext4_vn_rename(dir, "old-000002", dir, "apk-renamed", 0) < 0 ||
        ext4_lookup(dir, "old-000500", &old) < 0) goto fail;
    vnode_put(old); old = NULL;
    if (ext4_read_inode(sb, priv->inode_num, &inode) < 0 ||
        (inode.i_flags & EXT4_INDEX_FL)) goto fail;
    int block_group = -1, inode_group = -1;
    for (uint32_t g = 1; g < sb->groups_count; g++) {
        if (sb->group_descs[g].bg_flags & EXT4_BG_BLOCK_UNINIT) block_group = g;
        if (sb->group_descs[g].bg_flags & EXT4_BG_INODE_UNINIT) inode_group = g;
    }
    if (block_group < 0 || inode_group < 0) goto fail;
    sb->inode_group_rotor = (uint32_t)inode_group;
    if (ext4_vn_create(g_root, "lazy-group", 0644, &file) < 0 ||
        ext4_vn_truncate(file, 512 * 4096) < 0) goto fail;
    sb->block_group_rotor = (uint32_t)block_group;
    static char pages[64 * 4096], readback[sizeof(pages)];
    for (unsigned batch = 0; batch < 8; batch++) {
        memset(pages, 1 + batch, sizeof(pages));
        if (ext4_vn_writepages(file, batch * 64, pages, sizeof(pages)) < 0 ||
            ext4_vn_readpages(file, batch * 64, readback, sizeof(readback)) != sizeof(readback) ||
            memcmp(pages, readback, sizeof(pages))) goto fail;
    }
    if ((sb->group_descs[block_group].bg_flags & EXT4_BG_BLOCK_UNINIT) ||
        (sb->group_descs[inode_group].bg_flags & EXT4_BG_INODE_UNINIT)) goto fail;
    vnode_put(file); vnode_put(dir);
    host_report("HOST_INDEXED_LAZY_PASS\n");
    return;
fail:
    if (old) vnode_put(old);
    if (file) vnode_put(file);
    if (dir) vnode_put(dir);
    g_scenario_failed = 1;
    host_report("HOST_INDEXED_LAZY_FAIL\n");
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
        if (strcmp(spec, "writeback-eof") == 0) {
            do_writeback_size_race();
        } else if (strcmp(spec, "indexed-lazy") == 0) {
            do_indexed_lazy_regression();
        } else if (strcmp(spec, "desktop") == 0) {
            do_desktop_regression();
        } else if (strcmp(spec, "desktop-shutdown") == 0) {
            do_desktop_shutdown();
        } else if (strcmp(spec, "statfs") == 0) {
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
