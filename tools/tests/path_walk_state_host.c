/*
 * Host differential harness for the VFS path-resolution walk engine
 * (VFS_PATH_WALK_IMPL).
 *
 * Builds one simulated vnode tree + mount table + dentry cache, runs the C
 * reference walk (the pre-migration body of vnode_lookup_path_openat2,
 * reproduced below) and the Rust engine (kernel/fs/vfs/path_walk_state.rs,
 * linked as a host staticlib) over the same deterministic op stream of paths
 * and openat2 resolve flags, and compares the resolved vnode, the reported
 * errno and the resolved_out string.
 *
 * Both walks call the same simulated primitives (lookup, readlink, permission,
 * mount table, string layer), so the only difference under test is the walk
 * logic itself.
 *
 * Exit 0 on success; on mismatch prints the case and both results, exits 1.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXP 512
#define ENOENT 2
#define EAGAIN 11
#define EACCES 13
#define EXDEV 18
#define ENOTDIR 20
#define EINVAL 22
#define ENAMETOOLONG 36
#define ENOSYS 38
#define ELOOP 40

#define VFS_FT_REGULAR 1
#define VFS_FT_DIR 2
#define VFS_FT_SYMLINK 3
#define VNODE_MAGICLINK 0x1u
#define MAX_SYMLINKS 40
#define X_OK 1

#define RESOLVE_NO_XDEV 0x01
#define RESOLVE_NO_MAGICLINKS 0x02
#define RESOLVE_NO_SYMLINKS 0x04
#define RESOLVE_BENEATH 0x08
#define RESOLVE_IN_ROOT 0x10
#define RESOLVE_CACHED 0x20

/* ---- Rust ABI (mirrors fs/vfs/path_walk_state.h) ---- */
struct vfs_walk_ops {
    void (*vnode_get)(void *vn);
    void (*vnode_put)(void *vn);
    int (*vn_type)(const void *vn);
    uint64_t (*vn_ino)(const void *vn);
    uint32_t (*vn_link_flags)(const void *vn);
    void *(*vn_parent)(const void *vn);
    void *(*vn_mnt)(const void *vn);
    int (*has_lookup)(const void *dir);
    int (*lookup)(void *dir, const char *name, void **out);
    int (*has_readlink)(const void *vn);
    int (*readlink)(void *vn, char *buf, size_t sz);
    int (*permission)(void *vn, int mask);
    void *(*dcache_lookup)(void *dir, const char *name);
    void (*dcache_insert)(void *dir, const char *name, void *vn);
    void *(*find_mount)(const char *path);
    void *(*mount_parent)(void *mnt);
    const char *(*strip_mount_prefix)(const char *path, const void *mnt);
    void *(*mnt_root)(const void *mnt);
    const char *(*mnt_path)(const void *mnt);
    void *(*lookup_path)(void *mnt, const char *rel);
    int (*lookup_errno)(void);
    int (*normalize_with_root)(char *path, const char *root, size_t max_path_len);
    int (*is_beneath)(const char *start, const char *resolved);
    size_t max_path_len;
    size_t max_name_len;
    int max_symlinks;
};
extern void *a20_vfs_walk_path(const struct vfs_walk_ops *ops, const char *path,
                               const char *start, const char *root_path, uint64_t resolve,
                               char *resolved_out, size_t resolved_out_sz, int *lookup_err);

/* ---- simulated world ---- */
#define NVN 14
#define NMNT 2
#define NENT 24

typedef struct {
    uint64_t ino;
    int type;
    int parent;   /* vnode index, -1 for a root */
    int mnt;      /* mount index */
    int is_link;
    const char *target;
    int magic;
    int allow_x;
} SimNode;

typedef struct {
    int dir;
    const char *name;
    int child;
    int cached;
} SimEnt;

typedef struct {
    const char *path;
    int root;
    int parent;
} SimMnt;

static SimNode g_vn[NVN];
static SimEnt g_ent[NENT];
static int g_nent;
static SimMnt g_mnt[NMNT];

static void *V(int i) { return (void *)(uintptr_t)(0x1000 + i); }
static void *M(int i) { return (void *)(uintptr_t)(0x2000 + i); }
static int vidx(const void *p) { return (int)((uintptr_t)p - 0x1000); }
static int midx(const void *p) { return (int)((uintptr_t)p - 0x2000); }

static void add_ent(int dir, const char *name, int child, int cached)
{
    g_ent[g_nent].dir = dir;
    g_ent[g_nent].name = name;
    g_ent[g_nent].child = child;
    g_ent[g_nent].cached = cached;
    g_nent++;
}

static void world_init(void)
{
    memset(g_vn, 0, sizeof(g_vn));
    g_nent = 0;

    g_vn[0] = (SimNode){ 1, VFS_FT_DIR, -1, 0, 0, NULL, 0, 1 };            /* / */
    g_vn[1] = (SimNode){ 2, VFS_FT_DIR, 0, 0, 0, NULL, 0, 1 };             /* /a */
    g_vn[2] = (SimNode){ 3, VFS_FT_DIR, 1, 0, 0, NULL, 0, 1 };             /* /a/b */
    g_vn[3] = (SimNode){ 4, VFS_FT_REGULAR, 2, 0, 0, NULL, 0, 1 };         /* /a/b/f */
    g_vn[4] = (SimNode){ 5, VFS_FT_SYMLINK, 1, 0, 1, "b/f", 0, 1 };        /* /a/rel */
    g_vn[5] = (SimNode){ 6, VFS_FT_SYMLINK, 0, 0, 1, "/a/b", 0, 1 };       /* /abs */
    g_vn[6] = (SimNode){ 7, VFS_FT_SYMLINK, 0, 0, 1, "loop", 0, 1 };       /* /loop */
    g_vn[7] = (SimNode){ 8, VFS_FT_SYMLINK, 0, 0, 1, "a/f", 1, 1 };        /* /magic */
    g_vn[8] = (SimNode){ 1, VFS_FT_DIR, -1, 1, 0, NULL, 0, 1 };            /* /sub */
    g_vn[9] = (SimNode){ 9, VFS_FT_DIR, 8, 1, 0, NULL, 0, 1 };             /* /sub/s1 */
    g_vn[10] = (SimNode){ 10, VFS_FT_DIR, 0, 0, 0, NULL, 0, 0 };           /* /nofollow */
    g_vn[11] = (SimNode){ 11, VFS_FT_REGULAR, 0, 0, 0, NULL, 0, 1 };       /* /plain */
    g_vn[12] = (SimNode){ 12, VFS_FT_SYMLINK, 1, 0, 1, "/a/b/f", 0, 1 };   /* /a/abslink */
    g_vn[13] = (SimNode){ 13, VFS_FT_REGULAR, 8, 1, 0, NULL, 0, 1 };       /* /sub/s1/file */

    add_ent(0, "a", 1, 0);
    add_ent(0, "abs", 5, 1);
    add_ent(0, "loop", 6, 0);
    add_ent(0, "magic", 7, 0);
    add_ent(0, "nofollow", 10, 0);
    add_ent(0, "plain", 11, 1);
    add_ent(1, "b", 2, 0);
    add_ent(1, "rel", 4, 0);
    add_ent(1, "abslink", 12, 0);
    add_ent(2, "f", 3, 0);
    add_ent(8, "s1", 9, 0);
    add_ent(9, "file", 13, 0);

    g_mnt[0] = (SimMnt){ "/", 0, -1 };
    g_mnt[1] = (SimMnt){ "/sub", 8, 0 };
}

/* ---- simulated primitives (shared by both walks) ---- */

static void sim_get(void *vn) { (void)vn; }
static void sim_put(void *vn) { (void)vn; }
static int sim_type(const void *vn) { return g_vn[vidx(vn)].type; }
static uint64_t sim_ino(const void *vn) { return g_vn[vidx(vn)].ino; }
static uint32_t sim_link_flags(const void *vn)
{
    return g_vn[vidx(vn)].magic ? VNODE_MAGICLINK : 0u;
}
static void *sim_parent(const void *vn)
{
    int p = g_vn[vidx(vn)].parent;
    return p < 0 ? NULL : V(p);
}
static void *sim_mnt_of(const void *vn) { return M(g_vn[vidx(vn)].mnt); }
static int sim_has_lookup(const void *dir) { return g_vn[vidx(dir)].type == VFS_FT_DIR; }
static int sim_has_readlink(const void *vn) { return g_vn[vidx(vn)].is_link; }

static int sim_lookup(void *dir, const char *name, void **out)
{
    int d = vidx(dir);
    for (int i = 0; i < g_nent; i++) {
        if (g_ent[i].dir == d && strcmp(g_ent[i].name, name) == 0) {
            *out = V(g_ent[i].child);
            return 0;
        }
    }
    return -ENOENT;
}
static int sim_readlink(void *vn, char *buf, size_t sz)
{
    const char *t = g_vn[vidx(vn)].target;
    size_t n = strlen(t);
    if (n >= sz) return -ENAMETOOLONG;
    memcpy(buf, t, n);
    return (int)n;
}
static int sim_permission(void *vn, int mask)
{
    (void)mask;
    return g_vn[vidx(vn)].allow_x ? 0 : -EACCES;
}
static void *sim_dcache_lookup(void *dir, const char *name)
{
    int d = vidx(dir);
    for (int i = 0; i < g_nent; i++)
        if (g_ent[i].dir == d && g_ent[i].cached && strcmp(g_ent[i].name, name) == 0)
            return V(g_ent[i].child);
    return NULL;
}
static void sim_dcache_insert(void *dir, const char *name, void *vn)
{
    (void)dir; (void)name; (void)vn;
}
static void *sim_find_mount(const char *path)
{
    int best = -1;
    size_t best_len = 0;
    for (int i = 0; i < NMNT; i++) {
        size_t l = strlen(g_mnt[i].path);
        if (strncmp(path, g_mnt[i].path, l) != 0)
            continue;
        if (path[l] != '\0' && path[l] != '/')
            continue;
        if (l >= best_len) { best_len = l; best = i; }
    }
    return best < 0 ? NULL : M(best);
}
static void *sim_mount_parent(void *mnt)
{
    int p = g_mnt[midx(mnt)].parent;
    return p < 0 ? NULL : M(p);
}
static const char *sim_strip_mount_prefix(const char *path, const void *mnt)
{
    size_t len = strlen(g_mnt[midx(mnt)].path);
    if (strncmp(path, g_mnt[midx(mnt)].path, len) == 0) {
        const char *rest = path + len;
        if (*rest == '/') rest++;
        return rest;
    }
    return path;
}
static void *sim_mnt_root(const void *mnt) { return V(g_mnt[midx(mnt)].root); }
static const char *sim_mnt_path(const void *mnt) { return g_mnt[midx(mnt)].path; }
static void *sim_lookup_path(void *mnt, const char *rel)
{
    int cur = g_mnt[midx(mnt)].root;
    const char *p = rel;
    while (*p == '/') p++;
    char buf[MAXP];
    strncpy(buf, p, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *t = buf;
    while (*t) {
        char *slash = strchr(t, '/');
        if (slash) *slash = '\0';
        if (*t == '\0' || strcmp(t, ".") == 0) {
            /* skip */
        } else {
            void *out = NULL;
            if (sim_lookup(V(cur), t, &out) < 0)
                return NULL;
            cur = vidx(out);
        }
        if (!slash) break;
        t = slash + 1;
    }
    return V(cur);
}
static int sim_errno(void) { return 0; }

static int sim_normalize(char *path, const char *root, size_t max_path_len)
{
    (void)max_path_len;
    if (!path || path[0] != '/') return -EINVAL;
    if (!root || root[0] != '/') {
        /* fall back to the unfloored form */
        size_t rl = 0;
        char *parts[64];
        int n = 0;
        char input[MAXP];
        strncpy(input, path, sizeof(input) - 1);
        input[sizeof(input) - 1] = '\0';
        char *tok = input + 1;
        while (tok && *tok) {
            char *slash = strchr(tok, '/');
            if (slash) *slash = '\0';
            if (strcmp(tok, ".") == 0 || tok[0] == '\0') {
            } else if (strcmp(tok, "..") == 0) {
                if (n > 0) n--;
            } else if (n < 64) {
                parts[n++] = tok;
            } else {
                return -ENAMETOOLONG;
            }
            tok = slash ? slash + 1 : NULL;
        }
        (void)rl;
        if (n == 0) { strcpy(path, "/"); return 0; }
        char output[MAXP];
        size_t used = 0;
        output[used++] = '/';
        output[used] = '\0';
        for (int i = 0; i < n; i++) {
            size_t len = strlen(parts[i]);
            if (used + len + (i + 1 < n ? 1 : 0) >= sizeof(output)) return -ENAMETOOLONG;
            memcpy(output + used, parts[i], len);
            used += len;
            if (i + 1 < n) output[used++] = '/';
            output[used] = '\0';
        }
        strncpy(path, output, MAXP - 1);
        path[MAXP - 1] = '\0';
        return 0;
    }

    char input[MAXP];
    char output[MAXP];
    char *parts[64];
    int n = 0;
    size_t root_len = strlen(root);
    while (root_len > 1 && root[root_len - 1] == '/') root_len--;

    strncpy(input, path, sizeof(input) - 1);
    input[sizeof(input) - 1] = '\0';
    char *tok = input + 1;
    while (tok && *tok) {
        char *slash = strchr(tok, '/');
        if (slash) *slash = '\0';
        if (strcmp(tok, ".") == 0 || tok[0] == '\0') {
        } else if (strcmp(tok, "..") == 0) {
            size_t cur_len = 0;
            for (int i = 0; i < n; i++) cur_len += strlen(parts[i]) + 1;
            if (cur_len > root_len) n--;
        } else if (n < 64) {
            parts[n++] = tok;
        } else {
            return -ENAMETOOLONG;
        }
        tok = slash ? slash + 1 : NULL;
    }
    if (n == 0) { strcpy(path, "/"); return 0; }
    size_t used = 0;
    output[used++] = '/';
    output[used] = '\0';
    for (int i = 0; i < n; i++) {
        size_t len = strlen(parts[i]);
        if (used + len + (i + 1 < n ? 1 : 0) >= sizeof(output)) return -ENAMETOOLONG;
        memcpy(output + used, parts[i], len);
        used += len;
        if (i + 1 < n) output[used++] = '/';
        output[used] = '\0';
    }
    strncpy(path, output, MAXP - 1);
    path[MAXP - 1] = '\0';
    return 0;
}

static int sim_beneath(const char *start, const char *resolved)
{
    size_t len = strlen(start);
    while (len > 1 && start[len - 1] == '/') len--;
    if (len == 1 && start[0] == '/')
        return resolved[0] == '/';
    if (strcmp(resolved, start) == 0) return 1;
    if (strncmp(resolved, start, len) == 0 && (resolved[len] == '/' || resolved[len] == '\0'))
        return 1;
    return 0;
}

/* ---- C reference walk (pre-migration vnode_lookup_path_openat2) ---- */

static int vnode_is_mount_root(void *vn)
{
    void *mnt = sim_mnt_of(vn);
    void *root = sim_mnt_root(mnt);
    return mnt && root && sim_type(vn) == VFS_FT_DIR && sim_ino(vn) == sim_ino(root);
}

static int count_path_components(const char *path)
{
    int n = 0;
    const char *p = path;
    while (*p == '/') p++;
    while (*p) {
        n++;
        while (*p && *p != '/') p++;
        while (*p == '/') p++;
    }
    return n;
}

static void *ref_walk(const char *path, const char *start, const char *root_path,
                      uint64_t resolve, char *resolved_out, size_t resolved_out_sz,
                      int *lookup_err)
{
    if (!path || !start || !root_path || !resolved_out || resolved_out_sz == 0 || !lookup_err) {
        if (lookup_err) *lookup_err = -EINVAL;
        return NULL;
    }
    *lookup_err = 0;
    strncpy(resolved_out, path, resolved_out_sz - 1);
    resolved_out[resolved_out_sz - 1] = '\0';

    if (sim_normalize(resolved_out, root_path, MAXP) < 0) {
        *lookup_err = -ENAMETOOLONG;
        return NULL;
    }

    void *start_mnt = sim_find_mount(start);
    if (!start_mnt) start_mnt = sim_find_mount("/");

    void *mnt = sim_find_mount(resolved_out);
    if (!mnt || !sim_mnt_root(mnt)) {
        *lookup_err = -ENOENT;
        return NULL;
    }
    if ((resolve & RESOLVE_NO_XDEV) && mnt != start_mnt) {
        *lookup_err = -EXDEV;
        return NULL;
    }
    if ((resolve & RESOLVE_BENEATH) && !sim_beneath(start, resolved_out)) {
        *lookup_err = -EXDEV;
        return NULL;
    }

    void *cur = sim_mnt_root(mnt);
    sim_get(cur);
    if (strcmp(resolved_out, "/") == 0)
        return cur;

    char buf[MAXP];
    char link_target[MAXP];
    char cur_abs_path[MAXP];
    int symlink_depth = 0;

    strncpy(cur_abs_path, sim_mnt_path(mnt), sizeof(cur_abs_path) - 1);
    cur_abs_path[sizeof(cur_abs_path) - 1] = '\0';
    {
        size_t l = strlen(cur_abs_path);
        while (l > 1 && cur_abs_path[l - 1] == '/') cur_abs_path[--l] = '\0';
    }
    if (cur_abs_path[0] == '\0') { cur_abs_path[0] = '/'; cur_abs_path[1] = '\0'; }

    const char *rel = sim_strip_mount_prefix(resolved_out, mnt);
    strncpy(buf, rel, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *p = buf;
    while (*p == '/') p++;
    int depth = 0;

    while (*p) {
        char *sep = strchr(p, '/');
        if (sep) *sep = '\0';

        if (*p == '\0') {
        } else if (strcmp(p, ".") == 0) {
        } else if (strcmp(p, "..") == 0) {
            void *par = sim_parent(cur);
            if (depth > 0 && par && par != cur) {
                sim_get(par);
                sim_put(cur);
                cur = par;
                depth--;
                char *last = strrchr(cur_abs_path, '/');
                if (last) {
                    if (last == cur_abs_path) cur_abs_path[1] = '\0';
                    else *last = '\0';
                }
            } else if (vnode_is_mount_root(cur)) {
                void *parent_mnt = sim_mount_parent(sim_mnt_of(cur));
                if (parent_mnt) {
                    char new_path[MAXP];
                    char parent_rel[MAXP];
                    const char *mp_rel = sim_strip_mount_prefix(sim_mnt_path(sim_mnt_of(cur)), parent_mnt);
                    if (strcmp(mp_rel, "") == 0 || strcmp(mp_rel, "/") == 0) {
                        parent_rel[0] = '\0';
                    } else {
                        strncpy(parent_rel, mp_rel, sizeof(parent_rel) - 1);
                        parent_rel[sizeof(parent_rel) - 1] = '\0';
                        char *s = strrchr(parent_rel, '/');
                        if (s) {
                            if (s == parent_rel) parent_rel[1] = '\0';
                            else *s = '\0';
                        } else {
                            parent_rel[0] = '\0';
                        }
                    }
                    snprintf(new_path, sizeof(new_path), "%s%s%s",
                             strcmp(sim_mnt_path(parent_mnt), "/") == 0 ? "" : sim_mnt_path(parent_mnt),
                             parent_rel[0] ? "/" : "", parent_rel);
                    if (sim_normalize(new_path, root_path, MAXP) < 0) {
                        sim_put(cur); *lookup_err = -ENAMETOOLONG; return NULL;
                    }
                    if ((resolve & RESOLVE_BENEATH) && !sim_beneath(start, new_path)) {
                        sim_put(cur); *lookup_err = -EXDEV; return NULL;
                    }
                    if ((resolve & RESOLVE_NO_XDEV) && parent_mnt != start_mnt) {
                        sim_put(cur); *lookup_err = -EXDEV; return NULL;
                    }
                    sim_put(cur);
                    cur = sim_lookup_path(parent_mnt, parent_rel);
                    if (!cur) {
                        *lookup_err = sim_errno() ? sim_errno() : -ENOENT;
                        return NULL;
                    }
                    depth = count_path_components(parent_rel);
                    strncpy(cur_abs_path, new_path, sizeof(cur_abs_path) - 1);
                    cur_abs_path[sizeof(cur_abs_path) - 1] = '\0';
                }
            }
        } else {
            if (strlen(p) >= 256) { sim_put(cur); *lookup_err = -ENAMETOOLONG; return NULL; }
            if (sim_type(cur) != VFS_FT_DIR || !sim_has_lookup(cur)) {
                sim_put(cur); *lookup_err = -ENOTDIR; return NULL;
            }
            if (sim_permission(cur, X_OK) < 0) {
                sim_put(cur); *lookup_err = -EACCES; return NULL;
            }

            void *next = sim_dcache_lookup(cur, p);
            if (!next) {
                if (resolve & RESOLVE_CACHED) { sim_put(cur); *lookup_err = -EAGAIN; return NULL; }
                void *out = NULL;
                int r = sim_lookup(cur, p, &out);
                if (r < 0 || !out) { sim_put(cur); *lookup_err = r < 0 ? r : -ENOENT; return NULL; }
                next = out;
                sim_dcache_insert(cur, p, next);
            }

            void *parent = cur;
            cur = next;

            if (sim_type(cur) == VFS_FT_SYMLINK) {
                int follow = 1;
                if (resolve & RESOLVE_NO_SYMLINKS) follow = 0;
                if ((resolve & RESOLVE_NO_MAGICLINKS) && (sim_link_flags(cur) & VNODE_MAGICLINK)) follow = 0;
                if (!follow) { sim_put(parent); sim_put(cur); *lookup_err = -ELOOP; return NULL; }
                if (++symlink_depth > MAX_SYMLINKS) {
                    sim_put(parent); sim_put(cur); *lookup_err = -ELOOP; return NULL;
                }
                if (!sim_has_readlink(cur)) { sim_put(parent); sim_put(cur); *lookup_err = -ENOSYS; return NULL; }
                int len = sim_readlink(cur, link_target, sizeof(link_target) - 1);
                if (len < 0 || (size_t)len >= sizeof(link_target) - 1) {
                    sim_put(parent); sim_put(cur);
                    *lookup_err = len < 0 ? len : -ENAMETOOLONG;
                    return NULL;
                }
                link_target[len] = '\0';
                if (!len || ((resolve & RESOLVE_BENEATH) && link_target[0] == '/')) {
                    sim_put(parent); sim_put(cur);
                    *lookup_err = !len ? -ENOENT : -EXDEV;
                    return NULL;
                }

                void *old = cur;
                char rest[MAXP];
                int path_len;
                if (sep) path_len = snprintf(rest, sizeof(rest), "%s/%s", link_target, sep + 1);
                else path_len = snprintf(rest, sizeof(rest), "%s", link_target);

                char new_path[MAXP];
                if (path_len < 0 || (size_t)path_len >= sizeof(rest)) {
                    sim_put(old); sim_put(parent); *lookup_err = -ENAMETOOLONG; return NULL;
                }
                if (rest[0] == '/') {
                    const char *floor = (resolve & RESOLVE_IN_ROOT) ? start : root_path;
                    path_len = snprintf(new_path, sizeof(new_path), "%s%s",
                                        strcmp(floor, "/") == 0 ? "" : floor, rest);
                } else {
                    path_len = snprintf(new_path, sizeof(new_path), "%s/%s", cur_abs_path, rest);
                }
                if (path_len < 0 || (size_t)path_len >= sizeof(new_path)) {
                    sim_put(old); sim_put(parent); *lookup_err = -ENAMETOOLONG; return NULL;
                }
                const char *floor = (resolve & RESOLVE_IN_ROOT) ? start : root_path;
                if (sim_normalize(new_path, floor, MAXP) < 0) {
                    sim_put(old); sim_put(parent); *lookup_err = -ENAMETOOLONG; return NULL;
                }
                if ((resolve & RESOLVE_BENEATH) && !sim_beneath(start, new_path)) {
                    sim_put(old); sim_put(parent); *lookup_err = -EXDEV; return NULL;
                }
                void *new_mnt = sim_find_mount(new_path);
                if (!new_mnt || !sim_mnt_root(new_mnt)) {
                    sim_put(old); sim_put(parent); *lookup_err = -ENOENT; return NULL;
                }
                if ((resolve & RESOLVE_NO_XDEV) && new_mnt != start_mnt) {
                    sim_put(old); sim_put(parent); *lookup_err = -EXDEV; return NULL;
                }
                cur = sim_mnt_root(new_mnt);
                sim_get(cur);
                depth = 0;
                strncpy(cur_abs_path, sim_mnt_path(new_mnt), sizeof(cur_abs_path) - 1);
                cur_abs_path[sizeof(cur_abs_path) - 1] = '\0';
                {
                    size_t l = strlen(cur_abs_path);
                    while (l > 1 && cur_abs_path[l - 1] == '/') cur_abs_path[--l] = '\0';
                }
                if (cur_abs_path[0] == '\0') { cur_abs_path[0] = '/'; cur_abs_path[1] = '\0'; }
                strncpy(resolved_out, new_path, resolved_out_sz - 1);
                resolved_out[resolved_out_sz - 1] = '\0';
                const char *new_rel = sim_strip_mount_prefix(new_path, new_mnt);
                strncpy(buf, new_rel, sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = '\0';
                sim_put(old);
                sim_put(parent);
                p = buf;
                while (*p == '/') p++;
                continue;
            }

            if (sim_type(cur) == VFS_FT_DIR) {
                depth++;
                char tmp[MAXP];
                if (strcmp(cur_abs_path, "/") == 0)
                    snprintf(tmp, sizeof(tmp), "/%s", p);
                else
                    snprintf(tmp, sizeof(tmp), "%s/%s", cur_abs_path, p);
                strncpy(cur_abs_path, tmp, sizeof(cur_abs_path) - 1);
                cur_abs_path[sizeof(cur_abs_path) - 1] = '\0';
            }
            sim_put(parent);
        }

        if (sep) p = sep + 1;
        else break;
    }
    return cur;
}

/* ---- Rust-side vtable ---- */
static void r_get(void *vn) { sim_get(vn); }
static void r_put(void *vn) { sim_put(vn); }
static int r_type(const void *vn) { return sim_type(vn); }
static uint64_t r_ino(const void *vn) { return sim_ino(vn); }
static uint32_t r_link(const void *vn) { return sim_link_flags(vn); }
static void *r_parent(const void *vn) { return sim_parent(vn); }
static void *r_mnt(const void *vn) { return sim_mnt_of(vn); }
static int r_has_lookup(const void *d) { return sim_has_lookup(d); }
static int r_lookup(void *d, const char *n, void **o) { return sim_lookup(d, n, o); }
static int r_has_readlink(const void *v) { return sim_has_readlink(v); }
static int r_readlink(void *v, char *b, size_t s) { return sim_readlink(v, b, s); }
static int r_perm(void *v, int m) { return sim_permission(v, m); }
static void *r_dcl(void *d, const char *n) { return sim_dcache_lookup(d, n); }
static void r_dci(void *d, const char *n, void *v) { sim_dcache_insert(d, n, v); }
static void *r_fm(const char *p) { return sim_find_mount(p); }
static void *r_mp(void *m) { return sim_mount_parent(m); }
static const char *r_smp(const char *p, const void *m) { return sim_strip_mount_prefix(p, m); }
static void *r_mr(const void *m) { return sim_mnt_root(m); }
static const char *r_mpath(const void *m) { return sim_mnt_path(m); }
static void *r_lp(void *m, const char *r) { return sim_lookup_path(m, r); }
static int r_errno(void) { return sim_errno(); }
static int r_norm(char *p, const char *r, size_t m) { return sim_normalize(p, r, m); }
static int r_beneath(const char *s, const char *r) { return sim_beneath(s, r); }

static const struct vfs_walk_ops g_ops = {
    .vnode_get = r_get, .vnode_put = r_put, .vn_type = r_type, .vn_ino = r_ino,
    .vn_link_flags = r_link, .vn_parent = r_parent, .vn_mnt = r_mnt,
    .has_lookup = r_has_lookup, .lookup = r_lookup,
    .has_readlink = r_has_readlink, .readlink = r_readlink,
    .permission = r_perm, .dcache_lookup = r_dcl, .dcache_insert = r_dci,
    .find_mount = r_fm, .mount_parent = r_mp, .strip_mount_prefix = r_smp,
    .mnt_root = r_mr, .mnt_path = r_mpath, .lookup_path = r_lp, .lookup_errno = r_errno,
    .normalize_with_root = r_norm, .is_beneath = r_beneath,
    .max_path_len = MAXP, .max_name_len = 256, .max_symlinks = MAX_SYMLINKS,
};

/* ---- op stream ---- */
static uint64_t rng_state;
static uint64_t rnd(void)
{
    rng_state = rng_state * 6364136223846793005ull + 1442695040888963407ull;
    return rng_state >> 17;
}

static const char *g_comp[] = {
    "a", "b", "f", "rel", "abs", "loop", "magic", "sub", "s1", "file",
    "nofollow", "plain", "abslink", "nope", ".", "..", "",
};

static void gen_path(char *out, size_t cap)
{
    size_t used = 0;
    out[used++] = '/';
    int n = (int)(rnd() % 5);
    for (int i = 0; i < n; i++) {
        const char *c = g_comp[rnd() % (sizeof(g_comp) / sizeof(g_comp[0]))];
        size_t l = strlen(c);
        if (used + l + 2 >= cap) break;
        memcpy(out + used, c, l);
        used += l;
        if (rnd() % 4 != 0) out[used++] = '/';
    }
    out[used] = '\0';
}

static const uint64_t g_flags[] = {
    0, 0, 0, RESOLVE_NO_XDEV, RESOLVE_NO_MAGICLINKS, RESOLVE_NO_SYMLINKS,
    RESOLVE_BENEATH, RESOLVE_IN_ROOT, RESOLVE_CACHED,
    RESOLVE_NO_XDEV | RESOLVE_BENEATH, RESOLVE_NO_SYMLINKS | RESOLVE_CACHED,
};

int main(int argc, char **argv)
{
    unsigned rounds = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 200000;
    rng_state = 0x452821e638d01377ull;
    world_init();

    for (unsigned step = 0; step < rounds; step++) {
        char path[MAXP];
        gen_path(path, sizeof(path));
        uint64_t resolve = g_flags[rnd() % (sizeof(g_flags) / sizeof(g_flags[0]))];

        char ra[MAXP], rb[MAXP];
        int ea = 0, eb = 0;
        void *va = ref_walk(path, "/", "/", resolve, ra, sizeof(ra), &ea);
        void *vb = a20_vfs_walk_path(&g_ops, path, "/", "/", resolve, rb, sizeof(rb), &eb);

        int ia = va ? vidx(va) : -1;
        int ib = vb ? vidx(vb) : -1;
        if (ia != ib || ea != eb || strcmp(ra, rb) != 0) {
            printf("walk mismatch step %u path='%s' resolve=0x%llx\n"
                   "  C:    vn=%d err=%d out='%s'\n"
                   "  RUST: vn=%d err=%d out='%s'\n",
                   step, path, (unsigned long long)resolve, ia, ea, ra, ib, eb, rb);
            return 1;
        }
    }
    printf("vfs path walk C/Rust differential PASS (%u steps)\n", rounds);
    return 0;
}
