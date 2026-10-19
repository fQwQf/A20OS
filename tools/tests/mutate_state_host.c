/*
 * Host differential harness for the VFS directory-mutation orchestration engine
 * (VFS_MUTATE_IMPL).
 *
 * Drives the C reference bodies (the pre-migration vfs_unlink / vfs_rmdir /
 * vfs_rename_flags sequences from kernel/fs/vfs.c, reproduced below) and the
 * Rust engine (kernel/fs/vfs/mutate_state.rs, linked as a host staticlib) over
 * one shared deterministic op stream against the same simulated tree, and
 * compares the return value and the exact ordered log of side effects each
 * attempt produces.
 *
 * Exit 0 on success; on mismatch prints the op and both results, exits 1.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXP 512
#define MAXN 256

#define ENOENT 2
#define EACCES 13
#define EEXIST 17
#define EXDEV 18
#define ENOTDIR 20
#define EINVAL 22
#define ENAMETOOLONG 36
#define ENOSYS 38

#define VFS_FT_REGULAR 1
#define VFS_FT_DIR 2
#define FS_TYPE_RAMFS 4

#define W_OK 2
#define X_OK 1

#define RENAME_NOREPLACE 1
#define RENAME_EXCHANGE 2

struct vfs_mutate_ops {
    int (*join)(const char *cwd, const char *path, char *out, size_t outsz);
    int (*split_parent_name)(const char *path, char *parent, size_t parent_sz,
                             char *name, size_t name_sz, size_t max_path_len);
    int (*normalize_with_root)(char *path, const char *root, size_t max_path_len);
    int (*is_beneath)(const char *start, const char *resolved);
    void *(*find_mount)(const char *path);
    const char *(*strip_mount_prefix)(const char *path, const void *mnt);
    void *(*mnt_root)(const void *mnt);
    int (*mnt_type)(const void *mnt);
    void *(*lookup_path)(void *mnt, const char *rel);
    int (*lookup_errno)(void);
    void (*vnode_put)(void *vn);
    int (*vn_type)(const void *vn);
    uint64_t (*vn_ino)(const void *vn);
    size_t (*vn_size)(const void *vn);
    void *(*vn_mnt)(const void *vn);
    int (*has_lookup)(const void *dir);
    int (*lookup)(void *dir, const char *name, void **out);
    int (*has_unlink)(const void *dir);
    int (*has_rmdir)(const void *dir);
    int (*has_rename)(const void *dir);
    int (*do_unlink)(void *dir, const char *name);
    int (*do_rmdir)(void *dir, const char *name);
    int (*do_rename)(void *old_dir, const char *old_name, void *new_dir,
                     const char *new_name, uint32_t flags);
    int (*permission)(void *vn, int mask);
    int (*sticky_may_remove)(void *dir, void *victim);
    void (*notify_delete)(void *dir, const char *name, int is_dir);
    void (*notify_rename)(void *old_dir, const char *old_name, void *new_dir,
                          const char *new_name);
    void (*notify_self_delete)(void *vn);
    void (*quota_remove)(void *vn, size_t size);
    void (*dcache_invalidate)(void *dir, const char *name);
    void (*dcache_invalidate_all)(void);
    size_t max_path_len;
    size_t max_name_len;
};
extern int a20_vfs_unlink(const struct vfs_mutate_ops *, const char *, const char *);
extern int a20_vfs_rmdir(const struct vfs_mutate_ops *, const char *, const char *);
extern int a20_vfs_rename_flags(const struct vfs_mutate_ops *, const char *,
                                const char *, const char *, const char *, uint32_t);

/* ---- simulated tree ----
 *   /a      dir, allows W|X, has unlink/rmdir/rename
 *   /deny   dir, denies W|X
 *   /sticky dir, sticky (removal of any child is refused)
 *   /f      regular file in /
 *   /a/g    regular file in /a
 *   /ro     dir without unlink/rmdir/rename ops
 * plus a second mount at /mnt (EXDEV for a cross-mount rename).
 */
typedef struct {
    uint64_t ino;
    int type;
    size_t size;
    int deny;
    int sticky;
    int has_lookup, has_unlink, has_rmdir, has_rename;
    int mnt; /* 0 = "/", 1 = "/mnt" */
} Node;

typedef struct {
    int dir;
    const char *name;
    int child;
} Ent;

static Node g_node[8];
static Ent g_ent[8];
static int g_nent;

static void *V(int i) { return (void *)(uintptr_t)(0x1000 + i); }
static void *M(int i) { return (void *)(uintptr_t)(0x9000 + i); }
static int vi(const void *p) { return (int)((uintptr_t)p - 0x1000); }
static int mi(const void *p) { return (int)((uintptr_t)p - 0x9000); }

static const char *g_mnt_path[2] = { "/", "/mnt" };
static const int g_mnt_root[2] = { 0, 5 };
static const int g_mnt_type[2] = { 1, FS_TYPE_RAMFS };

static void world_init(void)
{
    memset(g_node, 0, sizeof(g_node));
    g_nent = 0;
    g_node[0] = (Node){ 1, VFS_FT_DIR, 0, 0, 0, 1, 1, 1, 1, 0 };  /* / */
    g_node[1] = (Node){ 2, VFS_FT_DIR, 0, 0, 0, 1, 1, 1, 1, 0 };  /* /a */
    g_node[2] = (Node){ 3, VFS_FT_DIR, 0, 1, 0, 1, 1, 1, 1, 0 };  /* /deny */
    g_node[3] = (Node){ 4, VFS_FT_DIR, 0, 0, 1, 1, 1, 1, 1, 0 };  /* /sticky */
    g_node[4] = (Node){ 5, VFS_FT_REGULAR, 42, 0, 0, 0, 0, 0, 0, 0 }; /* /f */
    g_node[5] = (Node){ 6, VFS_FT_DIR, 0, 0, 0, 1, 1, 1, 1, 1 };  /* /mnt */
    g_node[6] = (Node){ 7, VFS_FT_REGULAR, 7, 0, 0, 0, 0, 0, 0, 0 }; /* /a/g */
    g_node[7] = (Node){ 8, VFS_FT_DIR, 0, 0, 0, 1, 0, 0, 0, 0 };  /* /ro */
    g_ent[g_nent++] = (Ent){ 0, "a", 1 };
    g_ent[g_nent++] = (Ent){ 0, "deny", 2 };
    g_ent[g_nent++] = (Ent){ 0, "sticky", 3 };
    g_ent[g_nent++] = (Ent){ 0, "f", 4 };
    g_ent[g_nent++] = (Ent){ 0, "mnt", 5 };
    g_ent[g_nent++] = (Ent){ 0, "ro", 7 };
    g_ent[g_nent++] = (Ent){ 1, "g", 6 };
}

/* ---- shared instrumentation: the ordered log of effects ---- */
static char g_log[2048];
static size_t g_logn;

static void note(const char *s)
{
    size_t n = strlen(s);
    if (g_logn + n < sizeof(g_log)) {
        memcpy(g_log + g_logn, s, n);
        g_logn += n;
        g_log[g_logn] = '\0';
    }
}
static void log_reset(void) { g_logn = 0; g_log[0] = '\0'; }
static void note_name(const char *s)
{
    note(s);
    note(" ");
}

/* ---- primitives shared by both implementations ---- */

static int sim_join(const char *cwd, const char *path, char *out, size_t outsz)
{
    const char *base = (cwd && cwd[0]) ? cwd : "/";
    if (!path || !out || outsz == 0) return -EINVAL;
    if (path[0] == '/') {
        strncpy(out, path, outsz - 1);
        out[outsz - 1] = '\0';
        return 0;
    }
    if (strcmp(base, "/") == 0) snprintf(out, outsz, "/%s", path);
    else snprintf(out, outsz, "%s/%s", base, path);
    out[outsz - 1] = '\0';
    return 0;
}

static int sim_split(const char *path, char *parent, size_t psz, char *name,
                     size_t nsz, size_t max_path_len)
{
    (void)max_path_len;
    if (!path || !parent || !name || psz == 0 || nsz == 0) return -EINVAL;
    char tmp[MAXP];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    size_t l = strlen(tmp);
    while (l > 1 && tmp[l - 1] == '/') tmp[--l] = '\0';
    char *slash = strrchr(tmp, '/');
    const char *leaf = tmp;
    if (!slash) {
        parent[0] = '\0';
    } else {
        leaf = slash + 1;
        if (slash == tmp) {
            if (psz < 2) return -ENAMETOOLONG;
            parent[0] = '/';
            parent[1] = '\0';
        } else {
            size_t plen = (size_t)(slash - tmp);
            if (plen >= psz) return -ENAMETOOLONG;
            memcpy(parent, tmp, plen);
            parent[plen] = '\0';
        }
    }
    if (!leaf[0]) return -EINVAL;
    if (strlen(leaf) >= nsz) return -ENAMETOOLONG;
    strncpy(name, leaf, nsz - 1);
    name[nsz - 1] = '\0';
    return 0;
}

static int sim_norm(char *path, const char *root, size_t max_path_len)
{
    (void)root;
    (void)max_path_len;
    /* The generated paths are already absolute and normalised. */
    return path && path[0] == '/' ? 0 : -EINVAL;
}
static int sim_beneath(const char *s, const char *r)
{
    size_t len = strlen(s);
    while (len > 1 && s[len - 1] == '/') len--;
    if (len == 1 && s[0] == '/') return r[0] == '/';
    if (strcmp(r, s) == 0) return 1;
    if (strncmp(r, s, len) == 0 && (r[len] == '/' || r[len] == '\0')) return 1;
    return 0;
}
static void *sim_find_mount(const char *path)
{
    int best = -1;
    size_t bl = 0;
    for (int i = 0; i < 2; i++) {
        size_t l = strlen(g_mnt_path[i]);
        if (strncmp(path, g_mnt_path[i], l) != 0) continue;
        if (path[l] != '\0' && path[l] != '/') continue;
        if (l >= bl) { bl = l; best = i; }
    }
    return best < 0 ? NULL : M(best);
}
static const char *sim_strip(const char *path, const void *mnt)
{
    size_t l = strlen(g_mnt_path[mi(mnt)]);
    if (strncmp(path, g_mnt_path[mi(mnt)], l) == 0) {
        const char *rest = path + l;
        if (*rest == '/') rest++;
        return rest;
    }
    return path;
}
static void *sim_mnt_root(const void *mnt) { return V(g_mnt_root[mi(mnt)]); }
static int sim_mnt_type(const void *mnt) { return g_mnt_type[mi(mnt)]; }
static void *sim_lookup_path(void *mnt, const char *rel)
{
    int cur = g_mnt_root[mi(mnt)];
    const char *p = rel;
    while (*p == '/') p++;
    if (!*p) return V(cur);
    char buf[MAXP];
    strncpy(buf, p, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *t = buf;
    while (*t) {
        char *slash = strchr(t, '/');
        if (slash) *slash = '\0';
        if (*t && strcmp(t, ".") != 0) {
            void *out = NULL;
            int found = 0;
            for (int i = 0; i < g_nent; i++)
                if (g_ent[i].dir == cur && strcmp(g_ent[i].name, t) == 0) {
                    out = V(g_ent[i].child);
                    found = 1;
                }
            if (!found) return NULL;
            cur = vi(out);
        }
        if (!slash) break;
        t = slash + 1;
    }
    return V(cur);
}
static int sim_errno(void) { return 0; }
static void sim_put(void *v) { (void)v; }
static int sim_type(const void *v) { return g_node[vi(v)].type; }
static uint64_t sim_ino(const void *v) { return g_node[vi(v)].ino; }
static size_t sim_size(const void *v) { return g_node[vi(v)].size; }
static void *sim_vn_mnt(const void *v) { return M(g_node[vi(v)].mnt); }
static int sim_has_lookup(const void *d) { return g_node[vi(d)].has_lookup; }
static int sim_lookup(void *d, const char *n, void **o)
{
    int di = vi(d);
    for (int i = 0; i < g_nent; i++)
        if (g_ent[i].dir == di && strcmp(g_ent[i].name, n) == 0) {
            *o = V(g_ent[i].child);
            return 0;
        }
    return -ENOENT;
}
static int sim_has_unlink(const void *d) { return g_node[vi(d)].has_unlink; }
static int sim_has_rmdir(const void *d) { return g_node[vi(d)].has_rmdir; }
static int sim_has_rename(const void *d) { return g_node[vi(d)].has_rename; }
static int sim_do_unlink(void *d, const char *n)
{
    (void)d;
    char b[64];
    snprintf(b, sizeof(b), "unlink(%s)", n);
    note_name(b);
    return 0;
}
static int sim_do_rmdir(void *d, const char *n)
{
    (void)d;
    char b[64];
    snprintf(b, sizeof(b), "rmdir(%s)", n);
    note_name(b);
    return 0;
}
static int sim_do_rename(void *od, const char *on, void *nd, const char *nn, uint32_t f)
{
    (void)od;
    (void)nd;
    char b[96];
    snprintf(b, sizeof(b), "rename(%s->%s,f%u)", on, nn, (unsigned)f);
    note_name(b);
    return 0;
}
static int sim_perm(void *v, int mask)
{
    (void)mask;
    return g_node[vi(v)].deny ? EACCES : 0;
}
static int sim_sticky(void *d, void *victim)
{
    if (g_node[vi(d)].sticky || g_node[vi(victim)].sticky) {
        note("sticky-refused ");
        return -1;
    }
    return 0;
}
static void sim_notify_delete(void *d, const char *n, int is_dir)
{
    (void)d;
    char b[64];
    snprintf(b, sizeof(b), "del(%s,%d)", n, is_dir);
    note_name(b);
}
static void sim_notify_rename(void *od, const char *on, void *nd, const char *nn)
{
    (void)od;
    (void)nd;
    char b[80];
    snprintf(b, sizeof(b), "notify(%s->%s)", on, nn);
    note_name(b);
}
static void sim_notify_self(void *v) { (void)v; note_name("self"); }
static void sim_quota(void *v, size_t sz)
{
    (void)v;
    char b[48];
    snprintf(b, sizeof(b), "quota(%lu)", (unsigned long)sz);
    note_name(b);
}
static void sim_dc_inv(void *d, const char *n)
{
    (void)d;
    char b[64];
    snprintf(b, sizeof(b), "dcinv(%s)", n);
    note_name(b);
}
static void sim_dc_inv_all(void) { note_name("dcinv-all"); }

/* ---- C reference bodies ---- */

static int ref_unlink(const char *path)
{
    char resolved[MAXP];
    if (sim_join("/", path, resolved, sizeof(resolved)) < 0) return -EINVAL;
    void *mnt = sim_find_mount(resolved);
    if (!mnt || !sim_mnt_root(mnt)) return -ENOENT;
    const char *rel = sim_strip(resolved, mnt);
    char parent_path[MAXP], name[MAXN];
    int sr = sim_split(rel, parent_path, sizeof(parent_path), name, sizeof(name), MAXP);
    if (sr < 0) return sr;
    void *parent = sim_lookup_path(mnt, parent_path);
    if (!parent) return sim_errno() ? sim_errno() : -ENOENT;
    if (sim_type(parent) != VFS_FT_DIR) { sim_put(parent); return -ENOTDIR; }
    if (sim_perm(parent, W_OK | X_OK) < 0) { sim_put(parent); return -EACCES; }
    if (!sim_has_unlink(parent)) { sim_put(parent); return -ENOTDIR; }
    void *victim = NULL;
    if (sim_has_lookup(parent) && sim_lookup(parent, name, &victim) == 0 && victim) {
        int s = sim_sticky(parent, victim);
        if (s < 0) { sim_put(victim); sim_put(parent); return s; }
    }
    int r = sim_do_unlink(parent, name);
    if (r == 0) {
        sim_notify_delete(parent, name, 0);
        if (victim) sim_quota(victim, sim_size(victim));
        if (victim) sim_notify_self(victim);
        sim_dc_inv(parent, name);
    }
    sim_put(victim);
    sim_put(parent);
    return r;
}

static int ref_rmdir(const char *path)
{
    char resolved[MAXP];
    if (sim_join("/", path, resolved, sizeof(resolved)) < 0) return -EINVAL;
    char parent_path[MAXP], name[MAXN];
    int sr = sim_split(resolved, parent_path, sizeof(parent_path), name, sizeof(name), MAXP);
    if (sr < 0) return sr;
    void *mnt = sim_find_mount(parent_path);
    if (!mnt || !sim_mnt_root(mnt)) return -ENOENT;
    void *parent = sim_lookup_path(mnt, sim_strip(parent_path, mnt));
    if (!parent) return sim_errno() ? sim_errno() : -ENOENT;
    if (sim_type(parent) != VFS_FT_DIR) { sim_put(parent); return -ENOTDIR; }
    if (sim_perm(parent, W_OK | X_OK) < 0) { sim_put(parent); return -EACCES; }
    if (!sim_has_rmdir(parent)) { sim_put(parent); return -ENOSYS; }
    void *victim = NULL;
    if (sim_has_lookup(parent) && sim_lookup(parent, name, &victim) == 0 && victim) {
        int s = sim_sticky(parent, victim);
        if (s < 0) { sim_put(victim); sim_put(parent); return s; }
    }
    int r = sim_do_rmdir(parent, name);
    if (r == 0) {
        sim_notify_delete(parent, name, 1);
        if (victim) sim_notify_self(victim);
        sim_dc_inv(parent, name);
    }
    sim_put(victim);
    sim_put(parent);
    return r;
}

static int ref_rename(const char *old, const char *newpath, uint32_t flags)
{
    if (!old || !newpath) return -EINVAL;
    if (flags & ~(RENAME_NOREPLACE | RENAME_EXCHANGE)) return -EINVAL;
    if ((flags & RENAME_NOREPLACE) && (flags & RENAME_EXCHANGE)) return -EINVAL;

    char old_resolved[MAXP], new_resolved[MAXP];
    if (sim_join("/", old, old_resolved, sizeof(old_resolved)) < 0) return -EINVAL;
    if (sim_join("/", newpath, new_resolved, sizeof(new_resolved)) < 0) return -EINVAL;
    if (sim_norm(old_resolved, "/", MAXP) < 0) return -ENAMETOOLONG;
    if (sim_norm(new_resolved, "/", MAXP) < 0) return -ENAMETOOLONG;

    char old_parent[MAXP], old_name[MAXN], new_parent[MAXP], new_name[MAXN];
    int sr = sim_split(old_resolved, old_parent, sizeof(old_parent), old_name, sizeof(old_name), MAXP);
    if (sr < 0) return sr;
    sr = sim_split(new_resolved, new_parent, sizeof(new_parent), new_name, sizeof(new_name), MAXP);
    if (sr < 0) return sr;

    void *old_mnt = sim_find_mount(old_parent), *new_mnt = sim_find_mount(new_parent);
    if (!old_mnt || !new_mnt) return -ENOENT;
    if (old_mnt != new_mnt) return -EXDEV;

    void *old_dir = sim_lookup_path(old_mnt, sim_strip(old_parent, old_mnt));
    void *new_dir = sim_lookup_path(new_mnt, sim_strip(new_parent, new_mnt));
    if (!old_dir || !new_dir) {
        sim_put(old_dir);
        sim_put(new_dir);
        return sim_errno() ? sim_errno() : -ENOENT;
    }
    if (sim_type(old_dir) != VFS_FT_DIR || sim_type(new_dir) != VFS_FT_DIR) {
        sim_put(old_dir);
        sim_put(new_dir);
        return -ENOTDIR;
    }
    if (sim_perm(old_dir, W_OK | X_OK) < 0 || sim_perm(new_dir, W_OK | X_OK) < 0) {
        sim_put(old_dir);
        sim_put(new_dir);
        return -EACCES;
    }
    if (!sim_has_rename(old_dir)) {
        sim_put(old_dir);
        sim_put(new_dir);
        return -ENOSYS;
    }

    int old_is_dir = 0;
    void *old_victim = NULL;
    if (sim_has_lookup(old_dir) && sim_lookup(old_dir, old_name, &old_victim) == 0 && old_victim) {
        old_is_dir = sim_type(old_victim) == VFS_FT_DIR;
        int s = sim_sticky(old_dir, old_victim);
        sim_put(old_victim);
        if (s < 0) { sim_put(old_dir); sim_put(new_dir); return s; }
    }
    void *new_victim = NULL;
    if (sim_has_lookup(new_dir) && sim_lookup(new_dir, new_name, &new_victim) == 0 && new_victim) {
        if (flags & RENAME_NOREPLACE) {
            sim_put(new_victim);
            sim_put(old_dir);
            sim_put(new_dir);
            return -EEXIST;
        }
        int s = sim_sticky(new_dir, new_victim);
        if (s < 0) {
            sim_put(new_victim);
            sim_put(old_dir);
            sim_put(new_dir);
            return s;
        }
    }

    int r = sim_do_rename(old_dir, old_name, new_dir, new_name, flags);
    if (r == 0) {
        sim_notify_rename(old_dir, old_name, new_dir, new_name);
        void *om = sim_vn_mnt(old_dir);
        int reparent = om && sim_mnt_type(om) == FS_TYPE_RAMFS &&
                       sim_ino(old_dir) != sim_ino(new_dir) &&
                       (old_is_dir || ((flags & RENAME_EXCHANGE) && new_victim &&
                                       sim_type(new_victim) == VFS_FT_DIR));
        if (reparent) {
            sim_dc_inv_all();
        } else {
            sim_dc_inv(old_dir, old_name);
            sim_dc_inv(new_dir, new_name);
        }
    }
    sim_put(new_victim);
    sim_put(old_dir);
    sim_put(new_dir);
    return r;
}

/* ---- Rust-side vtable ---- */
static int r_join(const char *c, const char *p, char *o, size_t s) { return sim_join(c, p, o, s); }
static int r_split(const char *p, char *pa, size_t ps, char *n, size_t ns, size_t mpl) { return sim_split(p, pa, ps, n, ns, mpl); }
static int r_norm(char *p, const char *r, size_t m) { return sim_norm(p, r, m); }
static int r_beneath(const char *s, const char *r) { return sim_beneath(s, r); }
static void *r_fm(const char *p) { return sim_find_mount(p); }
static const char *r_strip(const char *p, const void *m) { return sim_strip(p, m); }
static void *r_mr(const void *m) { return sim_mnt_root(m); }
static int r_mt(const void *m) { return sim_mnt_type(m); }
static void *r_lp(void *m, const char *r) { return sim_lookup_path(m, r); }
static int r_errno(void) { return sim_errno(); }
static void r_put(void *v) { sim_put(v); }
static int r_type(const void *v) { return sim_type(v); }
static uint64_t r_ino(const void *v) { return sim_ino(v); }
static size_t r_size(const void *v) { return sim_size(v); }
static void *r_mnt(const void *v) { return sim_vn_mnt(v); }
static int r_has_lookup(const void *d) { return sim_has_lookup(d); }
static int r_lookup(void *d, const char *n, void **o) { return sim_lookup(d, n, o); }
static int r_has_unlink(const void *d) { return sim_has_unlink(d); }
static int r_has_rmdir(const void *d) { return sim_has_rmdir(d); }
static int r_has_rename(const void *d) { return sim_has_rename(d); }
static int r_do_unlink(void *d, const char *n) { return sim_do_unlink(d, n); }
static int r_do_rmdir(void *d, const char *n) { return sim_do_rmdir(d, n); }
static int r_do_rename(void *od, const char *on, void *nd, const char *nn, uint32_t f) { return sim_do_rename(od, on, nd, nn, f); }
static int r_perm(void *v, int m) { return sim_perm(v, m); }
static int r_sticky(void *d, void *v) { return sim_sticky(d, v); }
static void r_nd(void *d, const char *n, int i) { sim_notify_delete(d, n, i); }
static void r_nr(void *od, const char *on, void *nd, const char *nn) { sim_notify_rename(od, on, nd, nn); }
static void r_ns(void *v) { sim_notify_self(v); }
static void r_q(void *v, size_t s) { sim_quota(v, s); }
static void r_di(void *d, const char *n) { sim_dc_inv(d, n); }
static void r_dia(void) { sim_dc_inv_all(); }

static const struct vfs_mutate_ops g_ops = {
    .join = r_join, .split_parent_name = r_split, .normalize_with_root = r_norm,
    .is_beneath = r_beneath, .find_mount = r_fm, .strip_mount_prefix = r_strip,
    .mnt_root = r_mr, .mnt_type = r_mt, .lookup_path = r_lp, .lookup_errno = r_errno,
    .vnode_put = r_put, .vn_type = r_type, .vn_ino = r_ino, .vn_size = r_size,
    .vn_mnt = r_mnt, .has_lookup = r_has_lookup, .lookup = r_lookup,
    .has_unlink = r_has_unlink, .has_rmdir = r_has_rmdir, .has_rename = r_has_rename,
    .do_unlink = r_do_unlink, .do_rmdir = r_do_rmdir, .do_rename = r_do_rename,
    .permission = r_perm, .sticky_may_remove = r_sticky, .notify_delete = r_nd,
    .notify_rename = r_nr, .notify_self_delete = r_ns, .quota_remove = r_q,
    .dcache_invalidate = r_di, .dcache_invalidate_all = r_dia,
    .max_path_len = MAXP, .max_name_len = MAXN,
};

/* ---- op stream ---- */
static uint64_t rng_state;
static uint64_t rnd(void)
{
    rng_state = rng_state * 6364136223846793005ull + 1442695040888963407ull;
    return rng_state >> 17;
}

static const char *g_paths[] = { "/f", "/a", "/a/g", "/deny", "/sticky", "/ro",
                                 "/mnt", "/mnt/x", "/missing", "/f/x", "/" };
static const uint32_t g_flags[] = { 0, 0, RENAME_NOREPLACE, RENAME_EXCHANGE, 0x40,
                                    RENAME_NOREPLACE | RENAME_EXCHANGE };

int main(int argc, char **argv)
{
    unsigned rounds = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 200000;
    rng_state = 0xa4093822299f31d0ull;
    world_init();

    for (unsigned step = 0; step < rounds; step++) {
        const char *a = g_paths[rnd() % (sizeof(g_paths) / sizeof(g_paths[0]))];
        const char *b = g_paths[rnd() % (sizeof(g_paths) / sizeof(g_paths[0]))];
        uint32_t flags = g_flags[rnd() % (sizeof(g_flags) / sizeof(g_flags[0]))];
        unsigned op = (unsigned)(rnd() % 3);
        int ra = 0, rb = 0;
        char la[2048], lb[2048];

        log_reset();
        if (op == 0) ra = ref_unlink(a);
        else if (op == 1) ra = ref_rmdir(a);
        else ra = ref_rename(a, b, flags);
        strcpy(la, g_log);

        log_reset();
        if (op == 0) rb = a20_vfs_unlink(&g_ops, "/", a);
        else if (op == 1) rb = a20_vfs_rmdir(&g_ops, "/", a);
        else rb = a20_vfs_rename_flags(&g_ops, "/", "/", a, b, flags);
        strcpy(lb, g_log);

        if (ra != rb || strcmp(la, lb) != 0) {
            printf("mutate mismatch step %u op=%u path='%s' new='%s' flags=0x%x\n"
                   "  C:    rc=%d log='%s'\n"
                   "  RUST: rc=%d log='%s'\n",
                   step, op, a, b, flags, ra, la, rb, lb);
            return 1;
        }
    }
    printf("vfs mutate C/Rust differential PASS (%u steps)\n", rounds);
    return 0;
}
