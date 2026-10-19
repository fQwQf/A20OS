/*
 * Host differential harness for the VFS path/name state engine
 * (VFS_PATH_STATE_IMPL).
 *
 * Drives the C reference bodies (the pre-migration path.c algorithms,
 * reproduced below) and the Rust engine (kernel/fs/vfs/path_state.rs, linked as
 * a host staticlib) over one shared deterministic op stream of path strings,
 * comparing every result and every output buffer byte.
 *
 * Exit 0 on success; on mismatch prints the op and both results, exits 1.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PATH_LEN 512

#define EINVAL 22
#define ENAMETOOLONG 36

extern int a20_vfs_path_is_beneath(const char *start, const char *resolved);
extern int a20_vfs_path_join(const char *cwd, const char *path, char *out, size_t outsz);
extern void a20_vfs_path_trim_trailing_slashes(char *path);
extern int a20_vfs_path_split_parent_name(const char *path, char *parent, size_t parent_sz,
                                          char *name, size_t name_sz, size_t max_path_len);
extern int a20_vfs_path_normalize_absolute(char *path, size_t max_path_len);
extern int a20_vfs_path_normalize_absolute_with_root(char *path, const char *root,
                                                     size_t max_path_len);

static uint64_t rng_state;
static uint64_t rnd(void)
{
    rng_state = rng_state * 6364136223846793005ull + 1442695040888963407ull;
    return rng_state >> 17;
}

/* ---------------- C reference bodies ---------------- */

static void ref_trim(char *path)
{
    if (!path) return;
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/')
        path[--len] = '\0';
}

static int ref_beneath(const char *start, const char *resolved)
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

static int ref_join(const char *cwd, const char *path, char *out, size_t outsz)
{
    if (!path || !out || outsz == 0)
        return -EINVAL;
    const char *base = (cwd && cwd[0]) ? cwd : "/";
    if (path[0] == '/') {
        strncpy(out, path, outsz - 1);
        out[outsz - 1] = '\0';
        return 0;
    }
    size_t cwd_len = strlen(base);
    if (strcmp(base, "/") == 0)
        snprintf(out, outsz, "/%s", path);
    else if (cwd_len > 0 && base[cwd_len - 1] == '/')
        snprintf(out, outsz, "%s%s", base, path);
    else
        snprintf(out, outsz, "%s/%s", base, path);
    out[outsz - 1] = '\0';
    return 0;
}

static int ref_split(const char *path, char *parent, size_t parent_sz,
                     char *name, size_t name_sz)
{
    if (!path || !parent || !name || parent_sz == 0 || name_sz == 0)
        return -EINVAL;
    char tmp[MAX_PATH_LEN];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    ref_trim(tmp);
    char *slash = strrchr(tmp, '/');
    const char *leaf = tmp;
    if (!slash) {
        parent[0] = '\0';
    } else {
        leaf = slash + 1;
        if (slash == tmp) {
            if (parent_sz < 2) return -ENAMETOOLONG;
            parent[0] = '/'; parent[1] = '\0';
        } else {
            size_t plen = (size_t)(slash - tmp);
            if (plen >= parent_sz) return -ENAMETOOLONG;
            memcpy(parent, tmp, plen);
            parent[plen] = '\0';
        }
    }
    if (!leaf[0]) return -EINVAL;
    if (strlen(leaf) >= name_sz) return -ENAMETOOLONG;
    strncpy(name, leaf, name_sz - 1);
    name[name_sz - 1] = '\0';
    return 0;
}

static int ref_normalize(char *path, const char *root)
{
    if (!path || path[0] != '/') return -EINVAL;
    if (root && root[0] != '/') root = NULL;

    char input[MAX_PATH_LEN];
    char output[MAX_PATH_LEN];
    char *parts[64];
    int n = 0;
    size_t root_len = 0;
    if (root) {
        root_len = strlen(root);
        while (root_len > 1 && root[root_len - 1] == '/') root_len--;
    }

    strncpy(input, path, sizeof(input) - 1);
    input[sizeof(input) - 1] = '\0';

    char *tok = input + 1;
    while (tok && *tok) {
        char *slash = strchr(tok, '/');
        if (slash) *slash = '\0';
        if (strcmp(tok, ".") == 0 || tok[0] == '\0') {
        } else if (strcmp(tok, "..") == 0) {
            if (!root) {
                if (n > 0) n--;
            } else {
                size_t cur_len = 0;
                for (int i = 0; i < n; i++) cur_len += strlen(parts[i]) + 1;
                if (cur_len > root_len) n--;
            }
        } else if (n < (int)(sizeof(parts) / sizeof(parts[0]))) {
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
        if (used + len + (i + 1 < n ? 1 : 0) >= sizeof(output))
            return -ENAMETOOLONG;
        memcpy(output + used, parts[i], len);
        used += len;
        if (i + 1 < n) output[used++] = '/';
        output[used] = '\0';
    }
    strncpy(path, output, MAX_PATH_LEN - 1);
    path[MAX_PATH_LEN - 1] = '\0';
    return 0;
}

/* ---------------- scenario generation ---------------- */

static const char *g_comp[] = { "a", "bb", "ccc", ".", "..", "", "usr", "lib64", "x-y_z", "A" };

static void gen_path(char *out, size_t cap, int allow_relative)
{
    size_t used = 0;
    int absolute = allow_relative ? (int)(rnd() % 4 != 0) : 1;
    if (absolute) out[used++] = '/';
    int ncomp = (int)(rnd() % 7);
    for (int i = 0; i < ncomp; i++) {
        const char *c = g_comp[rnd() % (sizeof(g_comp) / sizeof(g_comp[0]))];
        size_t l = strlen(c);
        if (used + l + 2 >= cap) break;
        memcpy(out + used, c, l);
        used += l;
        if (rnd() % 3 != 0) out[used++] = '/';
    }
    if (used == 0) out[used++] = '/';
    out[used] = '\0';
}

static int cmp_buf(const char *tag, unsigned step, const char *a, const char *b)
{
    if (strcmp(a, b) != 0) {
        printf("%s mismatch step %u\n  C:    '%s'\n  RUST: '%s'\n", tag, step, a, b);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    unsigned rounds = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 200000;
    rng_state = 0x13198a2e03707344ull;

    for (unsigned step = 0; step < rounds; step++) {
        char p[MAX_PATH_LEN], cwd[MAX_PATH_LEN], root[MAX_PATH_LEN];
        gen_path(p, sizeof(p), 1);
        gen_path(cwd, sizeof(cwd), 0);
        gen_path(root, sizeof(root), 0);
        if (rnd() % 5 == 0) root[0] = '\0';

        switch (rnd() % 6) {
        case 0: { /* normalize */
            char a[MAX_PATH_LEN], b[MAX_PATH_LEN];
            strcpy(a, p); strcpy(b, p);
            int ra = ref_normalize(a, NULL);
            int rb = a20_vfs_path_normalize_absolute(b, MAX_PATH_LEN);
            if (ra != rb) { printf("normalize rc mismatch step %u: %d vs %d ('%s')\n", step, ra, rb, p); return 1; }
            if (ra == 0 && cmp_buf("normalize", step, a, b)) return 1;
            break;
        }
        case 1: { /* normalize with root */
            char a[MAX_PATH_LEN], b[MAX_PATH_LEN];
            strcpy(a, p); strcpy(b, p);
            int ra = ref_normalize(a, root);
            int rb = a20_vfs_path_normalize_absolute_with_root(b, root, MAX_PATH_LEN);
            if (ra != rb) { printf("normalize_root rc mismatch step %u: %d vs %d ('%s' root '%s')\n", step, ra, rb, p, root); return 1; }
            if (ra == 0 && cmp_buf("normalize_root", step, a, b)) return 1;
            break;
        }
        case 2: { /* join */
            char a[MAX_PATH_LEN], b[MAX_PATH_LEN];
            int ra = ref_join(cwd, p, a, MAX_PATH_LEN);
            int rb = a20_vfs_path_join(cwd, p, b, MAX_PATH_LEN);
            if (ra != rb) { printf("join rc mismatch step %u\n", step); return 1; }
            if (ra == 0 && cmp_buf("join", step, a, b)) return 1;
            break;
        }
        case 3: { /* split */
            char pa[MAX_PATH_LEN], pb[MAX_PATH_LEN], na[MAX_PATH_LEN], nb[MAX_PATH_LEN];
            int ra = ref_split(p, pa, MAX_PATH_LEN, na, MAX_PATH_LEN);
            int rb = a20_vfs_path_split_parent_name(p, pb, MAX_PATH_LEN, nb, MAX_PATH_LEN, MAX_PATH_LEN);
            if (ra != rb) { printf("split rc mismatch step %u: %d vs %d ('%s')\n", step, ra, rb, p); return 1; }
            if (ra == 0) {
                if (cmp_buf("split-parent", step, pa, pb)) return 1;
                if (cmp_buf("split-name", step, na, nb)) return 1;
            }
            break;
        }
        case 4: { /* beneath */
            int ra = ref_beneath(cwd, p);
            int rb = a20_vfs_path_is_beneath(cwd, p);
            if ((ra != 0) != (rb != 0)) {
                printf("beneath mismatch step %u: %d vs %d (start '%s' resolved '%s')\n", step, ra, rb, cwd, p);
                return 1;
            }
            break;
        }
        default: { /* trim */
            char a[MAX_PATH_LEN], b[MAX_PATH_LEN];
            strcpy(a, p); strcpy(b, p);
            ref_trim(a);
            a20_vfs_path_trim_trailing_slashes(b);
            if (cmp_buf("trim", step, a, b)) return 1;
            break;
        }
        }
    }

    printf("vfs path state C/Rust differential PASS (%u steps)\n", rounds);
    return 0;
}
