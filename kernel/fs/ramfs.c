#include "fs/ramfs.h"
#include "fs/fs.h"
#include "mm/mm.h"
#include "core/string.h"
#include "core/defs.h"

static vnode_ops_t g_ramfs_vnode_ops;
static vfile_ops_t g_ramfs_fops;

static vnode_t *ramfs_make_vnode(mount_t *mnt, inode_t *inode) {
    if (!inode) return NULL;

    vnode_t *vn = (vnode_t *)kmalloc(sizeof(vnode_t));
    if (!vn) return NULL;

    memset(vn, 0, sizeof(*vn));
    vn->ino = (uint64_t)inode->inum;
    if (inode->type == FT_DIRECTORY) vn->type = VFS_FT_DIR;
    else if (inode->type == FT_SYMLINK) vn->type = VFS_FT_SYMLINK;
    else vn->type = VFS_FT_REGULAR;

    if (vn->type == VFS_FT_DIR) vn->mode = S_IFDIR | 0755;
    else if (vn->type == VFS_FT_SYMLINK) vn->mode = S_IFLNK | 0777;
    else vn->mode = S_IFREG | 0755;

    vn->size = inode->size;
    vn->ref_count = 1;
    vn->mnt = mnt;
    vn->fs_data = (void *)inode;
    vn->ops = &g_ramfs_vnode_ops;
    return vn;
}

static int ramfs_vnode_lookup(vnode_t *dir, const char *name, vnode_t **out) {
    inode_t *dinode = (inode_t *)dir->fs_data;
    inode_t *found = NULL;
    int r = fs_inode_lookup(dinode, name, &found);
    if (r < 0) return r;

    *out = ramfs_make_vnode(dir->mnt, found);
    if (*out) {
        (*out)->parent = dir;
        dir->ref_count++;
    }
    return (*out) ? 0 : -ENOMEM;
}

static int ramfs_vnode_stat(vnode_t *vn, kstat_t *st) {
    inode_t *inode = (inode_t *)vn->fs_data;
    memset(st, 0, sizeof(*st));
    st->st_ino = inode->inum;
    st->st_mode = vn->mode;
    st->st_size = inode->size;
    st->st_nlink = 1;
    return 0;
}

static int ramfs_vnode_mkdir(vnode_t *dir, const char *name, int mode) {
    (void)mode;

    inode_t *dinode = (inode_t *)dir->fs_data;
    inode_t *child = alloc_inode(FT_DIRECTORY);
    if (!child) return -ENOMEM;

    child->parent = dinode;
    child->capacity = 64 * sizeof(dir_entry_t);
    child->data = kmalloc(child->capacity);
    if (!child->data) {
        child->ref_count = 0;
        return -ENOMEM;
    }

    memset(child->data, 0, child->capacity);
    add_dir_entry(child, ".", child->inum);
    add_dir_entry(child, "..", dinode->inum);
    add_dir_entry(dinode, name, child->inum);
    return 0;
}

static int ramfs_vnode_create(vnode_t *dir, const char *name, int mode, vnode_t **out) {
    (void)mode;

    inode_t *dinode = (inode_t *)dir->fs_data;
    inode_t *child = alloc_inode(FT_REGULAR);
    if (!child) return -ENOMEM;

    child->parent = dinode;
    child->capacity = 4096;
    child->data = kmalloc(child->capacity);
    if (!child->data) {
        child->ref_count = 0;
        return -ENOMEM;
    }

    add_dir_entry(dinode, name, child->inum);
    *out = ramfs_make_vnode(dir->mnt, child);
    if (*out) {
        (*out)->parent = dir;
        dir->ref_count++;
    }
    return *out ? 0 : -ENOMEM;
}

static void ramfs_vnode_release(vnode_t *vn) {
    vnode_put(vn->parent);
    kfree(vn);
}

static int ramfs_vnode_unlink(vnode_t *dir, const char *name) {
    inode_t *dinode = (inode_t *)dir->fs_data;
    dir_entry_t *entries = (dir_entry_t *)dinode->data;
    int n_entries = dinode->size / sizeof(dir_entry_t);

    for (int i = 0; i < n_entries; i++) {
        if (entries[i].name[0] != '\0' && strcmp(entries[i].name, name) == 0) {
            entries[i].name[0] = '\0';
            return 0;
        }
    }
    return -ENOENT;
}

static int ramfs_vnode_readlink(vnode_t *vn, char *buf, size_t sz) {
    inode_t *inode = (inode_t *)vn->fs_data;
    if (inode->type != FT_SYMLINK) return -EINVAL;

    size_t len = inode->size;
    if (len >= sz) len = sz - 1;
    if (len > 0 && inode->data) memcpy(buf, inode->data, len);
    buf[len] = '\0';
    return (int)len;
}

static int ramfs_vnode_symlink(vnode_t *dir, const char *name, const char *target) {
    inode_t *dinode = (inode_t *)dir->fs_data;
    if (dinode->type != FT_DIRECTORY) return -ENOTDIR;

    inode_t *child = alloc_inode(FT_SYMLINK);
    if (!child) return -ENOMEM;

    child->parent = dinode;
    size_t tlen = strlen(target);
    child->capacity = tlen + 1;
    child->data = kmalloc(child->capacity);
    if (!child->data) {
        child->ref_count = 0;
        return -ENOMEM;
    }

    memcpy(child->data, target, tlen + 1);
    child->size = tlen;
    add_dir_entry(dinode, name, child->inum);
    return 0;
}

static int ramfs_vnode_rename(vnode_t *old_dir, const char *old_name,
                              vnode_t *new_dir, const char *new_name) {
    inode_t *old_dinode = (inode_t *)old_dir->fs_data;
    inode_t *new_dinode = (inode_t *)new_dir->fs_data;
    if (old_dinode->type != FT_DIRECTORY || new_dinode->type != FT_DIRECTORY)
        return -ENOTDIR;

    dir_entry_t *old_entries = (dir_entry_t *)old_dinode->data;
    int n_old = old_dinode->size / sizeof(dir_entry_t);
    int old_idx = -1;
    int inum = 0;
    for (int i = 0; i < n_old; i++) {
        if (old_entries[i].name[0] != '\0' && strcmp(old_entries[i].name, old_name) == 0) {
            old_idx = i;
            inum = old_entries[i].inum;
            break;
        }
    }
    if (old_idx < 0) return -ENOENT;

    dir_entry_t *new_entries = (dir_entry_t *)new_dinode->data;
    int n_new = new_dinode->size / sizeof(dir_entry_t);
    int new_idx = -1;
    for (int i = 0; i < n_new; i++) {
        if (new_entries[i].name[0] != '\0' && strcmp(new_entries[i].name, new_name) == 0) {
            new_idx = i;
            break;
        }
    }

    if (new_idx >= 0) {
        new_entries[new_idx].inum = inum;
        memcpy(new_entries[new_idx].name, new_name, MAX_NAME_LEN);
    } else {
        add_dir_entry(new_dinode, new_name, inum);
    }

    old_entries[old_idx].name[0] = '\0';

    inode_t *moved = fs_find_inode_by_inum(inum);
    if (moved) moved->parent = new_dinode;
    return 0;
}

static int ramfs_vnode_rmdir(vnode_t *dir, const char *name) {
    inode_t *dinode = (inode_t *)dir->fs_data;
    dir_entry_t *entries = (dir_entry_t *)dinode->data;
    int n_entries = dinode->size / sizeof(dir_entry_t);

    for (int i = 0; i < n_entries; i++) {
        if (entries[i].name[0] != '\0' && strcmp(entries[i].name, name) == 0) {
            inode_t *child = fs_find_inode_by_inum(entries[i].inum);
            if (!child || child->type != FT_DIRECTORY) return -ENOTDIR;

            dir_entry_t *centries = (dir_entry_t *)child->data;
            int cn = child->size / sizeof(dir_entry_t);
            int active = 0;
            for (int j = 0; j < cn; j++) {
                if (centries[j].name[0] != '\0') active++;
            }
            if (active > 2) return -ENOTEMPTY;

            entries[i].name[0] = '\0';
            return 0;
        }
    }
    return -ENOENT;
}

static vnode_ops_t g_ramfs_vnode_ops = {
    .lookup = ramfs_vnode_lookup,
    .stat = ramfs_vnode_stat,
    .release = ramfs_vnode_release,
    .mkdir = ramfs_vnode_mkdir,
    .create = ramfs_vnode_create,
    .unlink = ramfs_vnode_unlink,
    .rmdir = ramfs_vnode_rmdir,
    .rename = ramfs_vnode_rename,
    .symlink = ramfs_vnode_symlink,
    .readlink = ramfs_vnode_readlink,
};

static int ramfs_fread(vfile_t *vf, char *buf, size_t count) {
    inode_t *inode = (inode_t *)vf->vnode->fs_data;
    if (vf->offset >= inode->size) return 0;

    size_t avail = inode->size - vf->offset;
    size_t n = count < avail ? count : avail;
    if (n > 0) {
        memcpy(buf, inode->data + vf->offset, n);
        vf->offset += n;
    }
    return (int)n;
}

static int ramfs_fwrite(vfile_t *vf, const char *buf, size_t count) {
    inode_t *inode = (inode_t *)vf->vnode->fs_data;
    if (inode->type == FT_DIRECTORY) return -EISDIR;

    size_t needed = vf->offset + count;
    if (needed > inode->capacity) {
        size_t new_cap = needed * 2;
        char *new_data = (char *)kmalloc(new_cap);
        if (!new_data) return -ENOMEM;
        if (inode->data) {
            memcpy(new_data, inode->data, inode->size);
            kfree(inode->data);
        }
        memset(new_data + inode->size, 0, new_cap - inode->size);
        inode->data = new_data;
        inode->capacity = new_cap;
    }

    memcpy(inode->data + vf->offset, buf, count);
    vf->offset += count;
    if (vf->offset > inode->size) inode->size = vf->offset;
    return (int)count;
}

static long ramfs_flseek(vfile_t *vf, long offset, int whence) {
    inode_t *inode = (inode_t *)vf->vnode->fs_data;
    long new_off;

    switch (whence) {
        case SEEK_SET: new_off = offset; break;
        case SEEK_CUR: new_off = (long)vf->offset + offset; break;
        case SEEK_END: new_off = (long)inode->size + offset; break;
        default: return -EINVAL;
    }

    if (new_off < 0) return -EINVAL;
    vf->offset = (size_t)new_off;
    return new_off;
}

static int ramfs_freaddir(vfile_t *vf, void *dirp, size_t count) {
    inode_t *inode = (inode_t *)vf->vnode->fs_data;
    if (inode->type != FT_DIRECTORY) return -ENOTDIR;

    dir_entry_t *entries = (dir_entry_t *)inode->data;
    int n_entries = inode->size / sizeof(dir_entry_t);
    char *out = (char *)dirp;
    size_t total = 0;

    int idx = (int)(vf->offset / sizeof(dir_entry_t));
    while (idx < n_entries) {
        dir_entry_t *de = &entries[idx];
        if (de->name[0] != '\0') {
            size_t namelen = strlen(de->name);
            size_t reclen = (offsetof(linux_dirent64_t, d_name) + namelen + 1 + 7) & ~7UL;
            if (total + reclen > count) break;

            linux_dirent64_t *d = (linux_dirent64_t *)(out + total);
            d->d_ino = (uint64_t)de->inum;
            d->d_off = (int64_t)(total + reclen);
            d->d_reclen = (uint16_t)reclen;
            d->d_type = DT_UNKNOWN;
            inode_t *child = fs_find_inode_by_inum(de->inum);
            if (child) {
                if (child->type == FT_DIRECTORY) d->d_type = DT_DIR;
                else if (child->type == FT_SYMLINK) d->d_type = DT_LNK;
                else if (child->type == FT_REGULAR) d->d_type = DT_REG;
            }
            memcpy(d->d_name, de->name, namelen + 1);
            total += reclen;
        }
        idx++;
        vf->offset += sizeof(dir_entry_t);
    }
    return (int)total;
}

static int ramfs_fclose(vfile_t *vf) {
    (void)vf;
    return 0;
}

static vfile_ops_t g_ramfs_fops = {
    .read = ramfs_fread,
    .write = ramfs_fwrite,
    .lseek = ramfs_flseek,
    .readdir = ramfs_freaddir,
    .close = ramfs_fclose,
};

vnode_t *ramfs_mount(mount_t *mnt) {
    return ramfs_make_vnode(mnt, fs_get_root());
}

vfile_t *ramfs_open_vnode(vnode_t *vn, int flags) {
    vfile_t *vf = (vfile_t *)kmalloc(sizeof(vfile_t));
    if (!vf) return NULL;

    memset(vf, 0, sizeof(*vf));
    vf->vnode = vn;
    vn->ref_count++;
    vf->flags = flags;
    vf->offset = (flags & O_APPEND) ? vn->size : 0;
    vf->ref_count = 1;
    vf->ops = &g_ramfs_fops;
    return vf;
}
