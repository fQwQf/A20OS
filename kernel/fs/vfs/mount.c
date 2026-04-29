#include "fs/vfs/mount.h"
#include "core/string.h"

#define MAX_MOUNTS 64

static mount_t g_mounts[MAX_MOUNTS];
static int g_nmounts;

void vfs_mount_table_init(void)
{
    memset(g_mounts, 0, sizeof(g_mounts));
    g_nmounts = 0;
}

int vfs_mount_count(void)
{
    return g_nmounts;
}

mount_t *vfs_mount_at(int index)
{
    if (index < 0 || index >= g_nmounts)
        return NULL;
    return &g_mounts[index];
}

mount_t *vfs_mount_alloc(void)
{
    if (g_nmounts >= MAX_MOUNTS)
        return NULL;
    mount_t *mnt = &g_mounts[g_nmounts++];
    memset(mnt, 0, sizeof(*mnt));
    return mnt;
}

void vfs_mount_remove(mount_t *mnt)
{
    if (!mnt)
        return;
    int idx = -1;
    for (int i = 0; i < g_nmounts; i++) {
        if (&g_mounts[i] == mnt) {
            idx = i;
            break;
        }
    }
    if (idx < 0)
        return;
    for (int i = idx; i < g_nmounts - 1; i++)
        g_mounts[i] = g_mounts[i + 1];
    memset(&g_mounts[g_nmounts - 1], 0, sizeof(g_mounts[g_nmounts - 1]));
    g_nmounts--;
}

mount_t *vfs_find_mount(const char *path)
{
    mount_t *best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < g_nmounts; i++) {
        size_t len = strlen(g_mounts[i].path);
        if (strncmp(path, g_mounts[i].path, len) == 0 &&
            (len == 1 || path[len] == '\0' || path[len] == '/') &&
            len > best_len) {
            best = &g_mounts[i];
            best_len = len;
        }
    }
    return best;
}

const char *vfs_strip_mount_prefix(const char *path, const mount_t *mnt)
{
    size_t len = strlen(mnt->path);
    if (strncmp(path, mnt->path, len) == 0) {
        const char *rest = path + len;
        if (*rest == '/')
            rest++;
        return rest;
    }
    return path;
}
