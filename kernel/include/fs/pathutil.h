#ifndef _PATHUTIL_H
#define _PATHUTIL_H

#include "core/types.h"
#include "core/string.h"
#include "core/consts.h"

/*
 * Build a path by replacing the last component of exec_path with suffix.
 * e.g. exec_path="/bin/ls", suffix="/lib/libc.so" -> "/bin/lib/libc.so"
 * Returns 0 on success, -ENOENT if exec_path has no '/'.
 */
static inline int path_build_sibling(const char *exec_path, const char *suffix,
                                     char *resolved, size_t resolved_size) {
    if (!exec_path || !suffix || !resolved || resolved_size == 0)
        return -ENOENT;

    int suffix_len = (int)strlen(suffix);
    for (int i = (int)strlen(exec_path) - 1; i >= 0; i--) {
        if (exec_path[i] != '/')
            continue;
        if ((size_t)(i + suffix_len + 1) >= resolved_size)
            continue;
        memcpy(resolved, exec_path, (size_t)i);
        strcpy(resolved + i, suffix);
        return 0;
    }
    return -ENOENT;
}

/*
 * Build a path rooted at the mount point of exec_path.
 * e.g. exec_path="/dev/vda/bin/ls", suffix="/lib/ld.so" -> "/dev/vda/lib/ld.so"
 * Returns 0 on success.
 */
static inline int path_build_mount_relative(const char *exec_path, const char *suffix,
                                            char *resolved, size_t resolved_size) {
    if (!exec_path || !suffix || suffix[0] != '/' ||
        !resolved || resolved_size == 0)
        return -ENOENT;

    int start = 0;
    if (exec_path[0] == '/') {
        start = 1;
        while (exec_path[start] && exec_path[start] != '/')
            start++;
    }

    size_t slen = strlen(suffix);
    if ((size_t)start + slen >= resolved_size)
        return -ENOENT;
    memcpy(resolved, exec_path, (size_t)start);
    memcpy(resolved + start, suffix, slen + 1);
    return 0;
}

/*
 * Count leading path components.  "/" -> 0, "/dev" -> 1, "/dev/vda/bin" -> 3.
 */
static inline int path_component_count(const char *path) {
    if (!path || path[0] != '/')
        return 0;
    int n = 0;
    const char *p = path + 1;
    while (*p) {
        if (*p == '/')
            p++;
        else {
            n++;
            while (*p && *p != '/')
                p++;
        }
    }
    return n;
}

#endif /* _PATHUTIL_H */
