#include "fs/vfs/path.h"

#include "core/consts.h"
#include "core/stdio.h"
#include "core/string.h"

int vfs_path_join(const char *cwd, const char *path, char *out, size_t outsz) {
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

void vfs_path_trim_trailing_slashes(char *path) {
    if (!path)
        return;
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/')
        path[--len] = '\0';
}

int vfs_path_split_parent_name(const char *path, char *parent, size_t parent_sz,
                               char *name, size_t name_sz) {
    if (!path || !parent || !name || parent_sz == 0 || name_sz == 0)
        return -EINVAL;

    char tmp[MAX_PATH_LEN];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    vfs_path_trim_trailing_slashes(tmp);

    char *slash = strrchr(tmp, '/');
    const char *leaf = tmp;
    if (!slash) {
        parent[0] = '\0';
    } else {
        leaf = slash + 1;
        if (slash == tmp) {
            if (parent_sz < 2)
                return -ENAMETOOLONG;
            parent[0] = '/';
            parent[1] = '\0';
        } else {
            size_t plen = (size_t)(slash - tmp);
            if (plen >= parent_sz)
                return -ENAMETOOLONG;
            memcpy(parent, tmp, plen);
            parent[plen] = '\0';
        }
    }

    if (!leaf[0])
        return -EINVAL;
    if (strlen(leaf) >= name_sz)
        return -ENAMETOOLONG;
    strncpy(name, leaf, name_sz - 1);
    name[name_sz - 1] = '\0';
    return 0;
}

int vfs_path_normalize_absolute(char *path) {
    if (!path || path[0] != '/')
        return -EINVAL;

    char input[MAX_PATH_LEN];
    char output[MAX_PATH_LEN];
    char *parts[64];
    int n = 0;

    strncpy(input, path, sizeof(input) - 1);
    input[sizeof(input) - 1] = '\0';

    char *tok = input + 1;
    while (tok && *tok) {
        char *slash = strchr(tok, '/');
        if (slash)
            *slash = '\0';

        if (strcmp(tok, ".") == 0 || tok[0] == '\0') {
            /* skip */
        } else if (strcmp(tok, "..") == 0) {
            if (n > 0)
                n--;
        } else if (n < (int)(sizeof(parts) / sizeof(parts[0]))) {
            parts[n++] = tok;
        } else {
            return -ENAMETOOLONG;
        }

        tok = slash ? slash + 1 : NULL;
    }

    if (n == 0) {
        strcpy(path, "/");
        return 0;
    }

    size_t used = 0;
    output[used++] = '/';
    output[used] = '\0';
    for (int i = 0; i < n; i++) {
        size_t len = strlen(parts[i]);
        if (used + len + (i + 1 < n ? 1 : 0) >= sizeof(output))
            return -ENAMETOOLONG;
        memcpy(output + used, parts[i], len);
        used += len;
        if (i + 1 < n)
            output[used++] = '/';
        output[used] = '\0';
    }

    strncpy(path, output, MAX_PATH_LEN - 1);
    path[MAX_PATH_LEN - 1] = '\0';
    return 0;
}
