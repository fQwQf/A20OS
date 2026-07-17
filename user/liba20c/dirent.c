#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../liba20rt/a20_types.h"
#include "../liba20rt/a20_fs.h"
#include "../liba20rt/a20_handle.h"

extern int __a20_to_errno(int a20_err);

struct __dirstream {
    a20_handle_t handle;
    a20_dirent_t buf;
    struct dirent ent;
    long pos;
};

DIR *opendir(const char *name)
{
    a20_path_open_args_t args;

    if (!name) {
        errno = EINVAL;
        return NULL;
    }

    args.size       = sizeof(args);
    args.version    = 1;
    args.dir        = A20_HANDLE_NULL;
    args.flags      = 0;
    args.rights     = A20_RIGHT_READ | A20_RIGHT_STAT;
    args.path       = (uint64_t)(uintptr_t)name;
    args.path_len   = (uint32_t)strlen(name);
    args.mode       = 0;
    args.out_handle = A20_HANDLE_NULL;

    a20_status_t r = a20_path_open(&args);
    if (A20_IS_ERROR(r)) {
        errno = __a20_to_errno((int)A20_ABS_ERROR(r));
        return NULL;
    }

    DIR *dir = (DIR *)malloc(sizeof(DIR));
    if (!dir) {
        errno = ENOMEM;
        a20_hdl_close(args.out_handle);
        return NULL;
    }

    dir->handle = args.out_handle;
    dir->pos   = 0;
    memset(&dir->buf, 0, sizeof(dir->buf));
    memset(&dir->ent, 0, sizeof(dir->ent));
    return dir;
}

struct dirent *readdir(DIR *dirp)
{
    uint32_t name_len;

    if (!dirp) {
        errno = EBADF;
        return NULL;
    }

    int64_t n = a20_path_readdir(dirp->handle, &dirp->buf, 1);
    if (n < 0) {
        errno = __a20_to_errno((int)(-n));
        return NULL;
    }
    if (n == 0) {
        errno = 0;
        return NULL;
    }

    dirp->ent.d_ino = 0;
    dirp->ent.d_type = (unsigned char)dirp->buf.type;

    name_len = dirp->buf.name_len;
    if (name_len >= sizeof(dirp->ent.d_name))
        name_len = sizeof(dirp->ent.d_name) - 1;
    memcpy(dirp->ent.d_name, dirp->buf.name, name_len);
    dirp->ent.d_name[name_len] = '\0';

    dirp->pos++;
    return &dirp->ent;
}

int closedir(DIR *dirp)
{
    if (!dirp) {
        errno = EBADF;
        return -1;
    }

    a20_status_t r = a20_hdl_close(dirp->handle);
    free(dirp);
    if (A20_IS_ERROR(r)) {
        errno = __a20_to_errno((int)A20_ABS_ERROR(r));
        return -1;
    }
    return 0;
}

void rewinddir(DIR *dirp)
{
    if (!dirp)
        return;
    dirp->pos = 0;
}

long telldir(DIR *dirp)
{
    if (!dirp) {
        errno = EBADF;
        return -1;
    }
    return dirp->pos;
}

void seekdir(DIR *dirp, long loc)
{
    if (!dirp)
        return;
    dirp->pos = loc;
}
