/*
 * Host differential harness for the procfs read/seek offset state and the
 * getdents64 directory-cursor state engine (PROCFS_STATE_IMPL).
 *
 * Drives the C reference loops (the pre-migration bodies from
 * kernel/fs/procfs/procfs.c, reproduced below) and the Rust engine
 * (kernel/fs/procfs/procfs_state.rs, linked as a host staticlib) over one shared
 * deterministic op stream, comparing the emitted dirent bytes and the resumed
 * cursor after every call.  Also exercises the seek/read offset plans.
 *
 * Exit 0 on success; on mismatch prints the op and both projections, exits 1.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIRENT_NAME_OFF 19u
#define DIRENT_SIZE     20u

#define PROCFS_DIR_FD     0u
#define PROCFS_DIR_FDINFO 1u
#define PROCFS_DIR_TASK   2u
#define PROCFS_DIR_STATIC 3u

typedef struct procfs_dir_entry {
    uint64_t key;
    int64_t  shown;
    const char *name;
    uint8_t  type_hint;
} procfs_dir_entry_t;

typedef int (*procfs_dir_next_fn)(void *ctx, uint64_t cursor,
                                  procfs_dir_entry_t *out);

extern int64_t a20_procfs_readdir_plan(uint32_t mode, uint64_t start,
                                       size_t count, void *ctx,
                                       procfs_dir_next_fn next, void *out,
                                       uint64_t *out_cursor);
extern int64_t a20_procfs_seek_plan(uint64_t content_len, uint64_t cur,
                                    int64_t off, int whence);
extern int64_t a20_procfs_read_plan(uint64_t content_len, uint64_t offset,
                                    size_t count, uint64_t *out_offset);

/* Mirror of vfs_dirent64_t (packed) so the harness can read fields back. */
typedef struct __attribute__((packed)) {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
} dirent_t;

#define NFD 64
#define NPID 24
#define NSTATIC 40

static int g_fd_occ[NFD];
static int g_pids[NPID];      /* raw (cursor) ids, ascending */
static int g_pids_shown[NPID];/* rendered ids */
static int g_npids;
static const char *g_names[NSTATIC];
static int g_names_dir[NSTATIC];
static int g_nnames;

static char g_namebuf[NSTATIC][24];

static uint64_t rng_state;
static uint64_t rnd(void)
{
    rng_state = rng_state * 6364136223846793005ull + 1442695040888963407ull;
    return rng_state >> 17;
}

/* ---------------- shared scenarios ---------------- */

static void scenario_init(void)
{
    memset(g_fd_occ, 0, sizeof(g_fd_occ));
    for (int i = 0; i < NFD; i++)
        if ((rnd() % 4) == 0)
            g_fd_occ[i] = 1;
    /* At least one occupied and one hole so the skip logic is exercised. */
    g_fd_occ[0] = 1;
    g_fd_occ[1] = 0;

    g_npids = 0;
    int pid = 1;
    while (g_npids < NPID) {
        if ((rnd() % 3) != 0) {
            g_pids[g_npids] = pid;
            g_pids_shown[g_npids] = pid * 7 + 1; /* namespace rendering */
            g_npids++;
        }
        pid += 1 + (int)(rnd() % 5);
    }

    g_nnames = 0;
    for (int i = 0; i < NSTATIC && g_nnames < NSTATIC; i++) {
        int kind = (int)(rnd() % 5);
        const char *n;
        int isdir;
        if (g_nnames == 0) { n = "."; isdir = 1; }
        else if (g_nnames == 1) { n = ".."; isdir = 1; }
        else if (kind == 0) { snprintf(g_namebuf[g_nnames], 24, "%d", 100 + g_nnames); n = g_namebuf[g_nnames]; isdir = 1; }
        else if (kind == 1) { snprintf(g_namebuf[g_nnames], 24, "f%02d", g_nnames); n = g_namebuf[g_nnames]; isdir = 0; }
        else if (kind == 2) { n = "fd"; isdir = 1; }
        else if (kind == 3) { n = "stat"; isdir = 0; }
        else { snprintf(g_namebuf[g_nnames], 24, "entry-%d", g_nnames); n = g_namebuf[g_nnames]; isdir = 0; }
        g_names[g_nnames] = n;
        g_names_dir[g_nnames] = isdir;
        g_nnames++;
    }
}

/* ---------------- C reference model ---------------- */

static int ref_dirent(uint8_t *out, size_t count, uint64_t *cursor,
                      uint32_t mode, uint64_t start)
{
    int index_mode = (mode == PROCFS_DIR_STATIC);
    size_t base = index_mode ? DIRENT_SIZE : DIRENT_NAME_OFF;
    uint64_t cur = start;
    size_t total = 0;

    while (total < count) {
        const char *name;
        char namebuf[24];
        uint64_t d_ino;
        uint8_t d_type;
        uint64_t next_cursor;
        int have = 1;

        if (!index_mode && cur < 2) {
            name = (cur == 0) ? "." : "..";
            d_ino = cur + 1;
            d_type = 4;
            next_cursor = cur + 1;
        } else if (index_mode) {
            if ((int)cur >= g_nnames) break;
            name = g_names[cur];
            d_ino = cur;
            d_type = g_names_dir[cur] ? 4 : 8;
            next_cursor = cur + 1;
        } else if (mode == PROCFS_DIR_TASK) {
            int best = 0, shown = 0;
            for (int i = 0; i < g_npids; i++) {
                if ((uint64_t)g_pids[i] < cur - 2) continue;
                if (!best || g_pids[i] < best) { best = g_pids[i]; shown = g_pids_shown[i]; }
            }
            if (!best) break;
            snprintf(namebuf, sizeof(namebuf), "%d", shown);
            name = namebuf;
            d_ino = (uint64_t)shown;
            d_type = 4;
            next_cursor = (uint64_t)best + 3;
        } else {
            int fd = (int)cur - 2;
            while (fd < NFD && !g_fd_occ[fd]) fd++;
            if (fd >= NFD) break;
            snprintf(namebuf, sizeof(namebuf), "%d", fd);
            name = namebuf;
            d_ino = (uint64_t)fd + 1;
            d_type = (mode == PROCFS_DIR_FD) ? 10 : 8;
            next_cursor = (uint64_t)fd + 3;
        }
        if (!have) break;

        size_t namelen = strlen(name);
        size_t reclen = (base + namelen + 1 + 7) & ~7UL;
        if (total + reclen > count) break;

        dirent_t *d = (dirent_t *)(out + total);
        d->d_ino = d_ino;
        d->d_off = index_mode ? (int64_t)(total + reclen) : (int64_t)next_cursor;
        d->d_reclen = (uint16_t)reclen;
        d->d_type = d_type;
        memcpy(d->d_name, name, namelen + 1);
        total += reclen;
        cur = next_cursor;
    }
    *cursor = cur;
    return (int)total;
}

/* ---------------- Rust-side lookups ---------------- */

static int next_fd(void *ctx, uint64_t cursor, procfs_dir_entry_t *out)
{
    (void)ctx;
    int fd = (int)cursor - 2;
    while (fd < NFD && !g_fd_occ[fd]) fd++;
    if (fd >= NFD) return 0;
    out->key = (uint64_t)fd;
    out->shown = (int64_t)fd;
    out->name = NULL;
    out->type_hint = 0;
    return 1;
}

static int next_task(void *ctx, uint64_t cursor, procfs_dir_entry_t *out)
{
    (void)ctx;
    int best = 0, shown = 0;
    for (int i = 0; i < g_npids; i++) {
        if ((uint64_t)g_pids[i] < cursor - 2) continue;
        if (!best || g_pids[i] < best) { best = g_pids[i]; shown = g_pids_shown[i]; }
    }
    if (!best) return 0;
    out->key = (uint64_t)best;
    out->shown = (int64_t)shown;
    out->name = NULL;
    out->type_hint = 0;
    return 1;
}

static int next_static(void *ctx, uint64_t cursor, procfs_dir_entry_t *out)
{
    (void)ctx;
    if ((int)cursor >= g_nnames) return 0;
    out->key = 0;
    out->shown = 0;
    out->name = g_names[cursor];
    out->type_hint = g_names_dir[cursor] ? 4 : 8;
    return 1;
}

static procfs_dir_next_fn next_for(uint32_t mode)
{
    switch (mode) {
    case PROCFS_DIR_TASK: return next_task;
    case PROCFS_DIR_STATIC: return next_static;
    default: return next_fd;
    }
}

static const char *mode_name(uint32_t mode)
{
    switch (mode) {
    case PROCFS_DIR_FD: return "fd";
    case PROCFS_DIR_FDINFO: return "fdinfo";
    case PROCFS_DIR_TASK: return "task";
    default: return "static";
    }
}

static void dump(const char *tag, const uint8_t *buf, int n, uint64_t cursor)
{
    printf("%s n=%d cursor=%llu\n", tag, n, (unsigned long long)cursor);
    int off = 0;
    while (off < n) {
        const dirent_t *d = (const dirent_t *)(buf + off);
        printf("  @%d ino=%llu off=%lld reclen=%u type=%u name=%s\n", off,
               (unsigned long long)d->d_ino, (long long)d->d_off, d->d_reclen,
               d->d_type, d->d_name);
        off += d->d_reclen;
    }
}

int main(int argc, char **argv)
{
    unsigned rounds = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 40000;
    rng_state = 0x243f6a8885a308d3ull;
    scenario_init();

    uint8_t ba[4096], bb[4096];
    static const size_t counts[] = { 16, 24, 48, 96, 256, 4096 };

    for (unsigned step = 0; step < rounds; step++) {
        uint32_t mode;
        uint64_t start;
        size_t count;

        switch (rnd() % 8) {
        case 0:
        case 1:
        case 2:
        case 3: { /* readdir batch comparison, walking to exhaustion */
            mode = (uint32_t)(rnd() % 4);
            count = counts[rnd() % (sizeof(counts) / sizeof(counts[0]))];
            if (mode == PROCFS_DIR_STATIC)
                start = rnd() % (g_nnames + 2);
            else if (mode == PROCFS_DIR_TASK)
                start = (rnd() % 3 == 0) ? (rnd() % 2) : (uint64_t)g_pids[rnd() % g_npids] + 2;
            else
                start = (rnd() % 3 == 0) ? (rnd() % 2) : (uint64_t)(rnd() % NFD) + 2;

            for (int round = 0; round < 8; round++) {
                uint64_t ca = start, cb = start;
                /* The bytes between a name's NUL and its 8-byte reclen bound
                 * are unspecified (neither engine writes them); zero both
                 * buffers so the comparison is over the real dirent contract. */
                memset(ba, 0, sizeof(ba));
                memset(bb, 0, sizeof(bb));
                int na = ref_dirent(ba, count, &ca, mode, start);
                int64_t nb = a20_procfs_readdir_plan(mode, start, count, NULL,
                                                     next_for(mode), bb, &cb);
                if (na != (int)nb || ca != cb || memcmp(ba, bb, (size_t)na) != 0) {
                    printf("readdir mismatch step %u mode=%s count=%zu start=%llu\n",
                           step, mode_name(mode), count, (unsigned long long)start);
                    dump("C:", ba, na, ca);
                    dump("RUST:", bb, (int)nb, cb);
                    return 1;
                }
                if (na == 0) break;
                start = ca;
            }
            break;
        }
        case 4:
        case 5: { /* lseek plan */
            uint64_t len = rnd() % 200;
            uint64_t cur = rnd() % 200;
            int64_t off = (int64_t)(rnd() % 400) - 200;
            int whence = (int)(rnd() % 4);
            int64_t got = a20_procfs_seek_plan(len, cur, off, whence);
            int64_t want;
            int valid = 1;
            switch (whence) {
            case 0: want = off; break;
            case 1: want = (int64_t)cur + off; break;
            case 2: want = (int64_t)len + off; break;
            default: valid = 0; want = -22; break;
            }
            if (valid && want < 0) want = 0;
            if (got != want) {
                printf("seek mismatch step %u len=%llu cur=%llu off=%lld whence=%d got=%lld want=%lld\n",
                       step, (unsigned long long)len, (unsigned long long)cur,
                       (long long)off, whence, (long long)got, (long long)want);
                return 1;
            }
            break;
        }
        default: { /* read plan */
            uint64_t len = rnd() % 200;
            uint64_t off = rnd() % 220;
            size_t count = rnd() % 128;
            uint64_t got_off = off;
            int64_t got = a20_procfs_read_plan(len, off, count, &got_off);
            int64_t want;
            uint64_t want_off;
            if (off >= len) { want = 0; want_off = off; }
            else {
                uint64_t avail = len - off;
                uint64_t n = ((uint64_t)count < avail) ? (uint64_t)count : avail;
                want = (int64_t)n;
                want_off = off + n;
            }
            if (got != want || got_off != want_off) {
                printf("read mismatch step %u len=%llu off=%llu count=%zu got=%lld/%llu want=%lld/%llu\n",
                       step, (unsigned long long)len, (unsigned long long)off,
                       count, (long long)got, (unsigned long long)got_off,
                       (long long)want, (unsigned long long)want_off);
                return 1;
            }
            break;
        }
        }
    }

    printf("procfs state C/Rust differential PASS (%u steps)\n", rounds);
    return 0;
}
