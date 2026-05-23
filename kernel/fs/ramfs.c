#include "fs/ramfs.h"
#include "fs/file.h"
#include "mm/mm.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/panic.h"
#include "core/defs.h"
#include "proc/proc.h"

#define RAMFS_MAX_INODES       4096
#define RAMFS_MAX_DIR_ENTRIES   256

typedef struct ramfs_inode {
    int inum;
    int type;
    int ref_count;
    int nlink;
    size_t size;
    char *data;
    size_t capacity;
    struct ramfs_inode *parent;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
} ramfs_inode_t;

typedef struct {
    int inum;
    char name[MAX_NAME_LEN];
} ramfs_dir_entry_t;

static ramfs_inode_t g_inode_table[RAMFS_MAX_INODES];
static int g_next_inum = 1;
static int g_ramfs_ready = 0;

static vnode_ops_t g_ramfs_vnode_ops;
static vfile_ops_t g_ramfs_fops;

static ramfs_inode_t *ramfs_find_inode_by_inum(int inum) {
    for (int i = 0; i < RAMFS_MAX_INODES; i++) {
        if (g_inode_table[i].ref_count > 0 && g_inode_table[i].inum == inum)
            return &g_inode_table[i];
    }
    return NULL;
}

static ramfs_inode_t *ramfs_alloc_inode(int type) {
    for (int i = 0; i < RAMFS_MAX_INODES; i++) {
        if (g_inode_table[i].ref_count == 0) {
            memset(&g_inode_table[i], 0, sizeof(g_inode_table[i]));
            g_inode_table[i].inum = g_next_inum++;
            g_inode_table[i].type = type;
            g_inode_table[i].ref_count = 1;
            g_inode_table[i].nlink = 1;
            task_t *cur = proc_current();
            g_inode_table[i].uid = cur ? (uint32_t)cur->cred.fsuid : 0;
            g_inode_table[i].gid = cur ? (uint32_t)cur->cred.fsgid : 0;
            if (type == FT_DIRECTORY) g_inode_table[i].mode = S_IFDIR | 0755;
            else if (type == FT_SYMLINK) g_inode_table[i].mode = S_IFLNK | 0777;
            else g_inode_table[i].mode = S_IFREG | 0644;
            return &g_inode_table[i];
        }
    }
    return NULL;
}

static void ramfs_free_inode(ramfs_inode_t *inode) {
    if (!inode || inode == &g_inode_table[0])
        return;
    if (inode->data)
        kfree(inode->data);
    memset(inode, 0, sizeof(*inode));
}

static void ramfs_maybe_free_unlinked_inode(ramfs_inode_t *inode) {
    if (!inode || inode == &g_inode_table[0])
        return;
    if (inode->nlink == 0 && inode->ref_count <= 1)
        ramfs_free_inode(inode);
}

static ramfs_dir_entry_t *ramfs_dir_entries(ramfs_inode_t *dir) {
    return (ramfs_dir_entry_t *)dir->data;
}

static int ramfs_dir_entry_count(ramfs_inode_t *dir) {
    return (int)(dir->size / sizeof(ramfs_dir_entry_t));
}

static int ramfs_add_dir_entry(ramfs_inode_t *dir, const char *name, int inum) {
    int count = ramfs_dir_entry_count(dir);
    ramfs_dir_entry_t *entries = ramfs_dir_entries(dir);

    for (int i = 0; i < count; i++) {
        if (entries[i].name[0] == '\0') {
            strncpy(entries[i].name, name, MAX_NAME_LEN - 1);
            entries[i].name[MAX_NAME_LEN - 1] = '\0';
            entries[i].inum = inum;
            return 0;
        }
    }

    if (count >= RAMFS_MAX_DIR_ENTRIES) return -ENOSPC;

    size_t needed = (count + 1) * sizeof(ramfs_dir_entry_t);
    if (needed > dir->capacity) {
        size_t new_cap = needed * 2;
        char *new_data = kmalloc(new_cap);
        if (!new_data) return -ENOMEM;
        if (dir->data) {
            memcpy(new_data, dir->data, dir->size);
            kfree(dir->data);
        }
        memset(new_data + dir->size, 0, new_cap - dir->size);
        dir->data = new_data;
        dir->capacity = new_cap;
    }

    entries = ramfs_dir_entries(dir);
    strncpy(entries[count].name, name, MAX_NAME_LEN - 1);
    entries[count].name[MAX_NAME_LEN - 1] = '\0';
    entries[count].inum = inum;
    dir->size = needed;
    return 0;
}

static ramfs_inode_t *ramfs_find_in_dir(ramfs_inode_t *dir, const char *name) {
    ramfs_dir_entry_t *entries = ramfs_dir_entries(dir);
    int count = ramfs_dir_entry_count(dir);
    for (int i = 0; i < count; i++) {
        if (entries[i].name[0] != '\0' && strcmp(entries[i].name, name) == 0)
            return ramfs_find_inode_by_inum(entries[i].inum);
    }
    return NULL;
}

static int ramfs_inode_lookup(ramfs_inode_t *dir, const char *name, ramfs_inode_t **out) {
    if (!dir || dir->type != FT_DIRECTORY) return -ENOTDIR;
    ramfs_inode_t *found = ramfs_find_in_dir(dir, name);
    if (!found) return -ENOENT;
    *out = found;
    return 0;
}

static void ramfs_init_storage(void) {
    memset(g_inode_table, 0, sizeof(g_inode_table));
    g_next_inum = 1;

    ramfs_inode_t *root = &g_inode_table[0];
    root->inum = 0;
    root->type = FT_DIRECTORY;
    root->ref_count = 1;
    root->nlink = 2;
    root->mode = S_IFDIR | 0755;
    root->uid = 0;
    root->gid = 0;
    root->capacity = RAMFS_MAX_DIR_ENTRIES * sizeof(ramfs_dir_entry_t);
    root->data = kmalloc(root->capacity);
    root->parent = root;
    if (!root->data) panic("ramfs_init: no memory for root dir");
    memset(root->data, 0, root->capacity);

    ramfs_add_dir_entry(root, ".", 0);
    ramfs_add_dir_entry(root, "..", 0);

    const char *text = "Hello from A20OS!\n"
        "A20 is an abbreviation of AAAAAAAAAAAAAAAAAAAAOS.\n"
        "This is a sample text file for testing.\n"
        "You can try: cat /hello.txt\n"
        "Supported commands: ls, cat, mkdir, rm, cp, pwd, cd, echo, help\n";
    size_t tlen = strlen(text);
    ramfs_inode_t *f = ramfs_alloc_inode(FT_REGULAR);
    if (f) {
        f->parent = root;
        f->capacity = tlen + 64;
        f->data = kmalloc(f->capacity);
        if (f->data) {
            memcpy(f->data, text, tlen);
            f->size = tlen;
        }
        ramfs_add_dir_entry(root, "hello.txt", f->inum);
    }

    g_ramfs_ready = 1;
    printf("[RAMFS] Initialized, root inode 0\n");
}

static ramfs_inode_t *ramfs_get_root(void) {
    if (!g_ramfs_ready)
        ramfs_init_storage();
    return &g_inode_table[0];
}

static vnode_t *ramfs_make_vnode(mount_t *mnt, ramfs_inode_t *inode) {
    if (!inode) return NULL;

    vnode_t *vn = (vnode_t *)kmalloc(sizeof(vnode_t));
    if (!vn) return NULL;
    inode->ref_count++;

    memset(vn, 0, sizeof(*vn));
    vn->ino = (uint64_t)inode->inum;
    if (inode->type == FT_DIRECTORY) vn->type = VFS_FT_DIR;
    else if (inode->type == FT_SYMLINK) vn->type = VFS_FT_SYMLINK;
    else vn->type = VFS_FT_REGULAR;

    vn->mode = inode->mode;
    vn->uid = inode->uid;
    vn->gid = inode->gid;

    vn->size = inode->size;
    vnode_ref_init(vn, 1);
    vn->mnt = mnt;
    vn->fs_data = (void *)inode;
    vn->ops = &g_ramfs_vnode_ops;
    return vn;
}

static int ramfs_vnode_lookup(vnode_t *dir, const char *name, vnode_t **out) {
    ramfs_inode_t *dinode = (ramfs_inode_t *)dir->fs_data;
    ramfs_inode_t *found = NULL;
    int r = ramfs_inode_lookup(dinode, name, &found);
    if (r < 0) return r;

    *out = ramfs_make_vnode(dir->mnt, found);
    if (*out) {
        (*out)->parent = dir;
        vnode_get(dir);
    }
    return (*out) ? 0 : -ENOMEM;
}

static int ramfs_vnode_stat(vnode_t *vn, kstat_t *st) {
    ramfs_inode_t *inode = (ramfs_inode_t *)vn->fs_data;
    memset(st, 0, sizeof(*st));
    st->st_ino = inode->inum;
    st->st_mode = inode->mode;
    st->st_uid = inode->uid;
    st->st_gid = inode->gid;
    st->st_size = inode->size;
    st->st_nlink = inode->nlink > 0 ? (uint32_t)inode->nlink : 1;
    return 0;
}

static int ramfs_vnode_mkdir(vnode_t *dir, const char *name, int mode) {
    ramfs_inode_t *dinode = (ramfs_inode_t *)dir->fs_data;
    ramfs_inode_t *child = ramfs_alloc_inode(FT_DIRECTORY);
    if (!child) return -ENOMEM;
    child->mode = S_IFDIR | (mode & 07777);
    if (dinode->mode & S_ISGID) {
        child->gid = dinode->gid;
        child->mode |= S_ISGID;
    }

    child->parent = dinode;
    child->capacity = 64 * sizeof(ramfs_dir_entry_t);
    child->data = kmalloc(child->capacity);
    if (!child->data) {
        child->ref_count = 0;
        return -ENOMEM;
    }

    memset(child->data, 0, child->capacity);
    ramfs_add_dir_entry(child, ".", child->inum);
    ramfs_add_dir_entry(child, "..", dinode->inum);
    int r = ramfs_add_dir_entry(dinode, name, child->inum);
    if (r < 0) {
        ramfs_free_inode(child);
        return r;
    }
    dinode->nlink++;
    return 0;
}

static int ramfs_vnode_create(vnode_t *dir, const char *name, int mode, vnode_t **out) {
    ramfs_inode_t *dinode = (ramfs_inode_t *)dir->fs_data;
    ramfs_inode_t *child = ramfs_alloc_inode(FT_REGULAR);
    if (!child) return -ENOMEM;
    if (mode & S_IFMT) {
        child->mode = (mode & S_IFMT) | (mode & 07777);
    } else {
        child->mode = S_IFREG | (mode & 07777);
    }
    if (dinode->mode & S_ISGID)
        child->gid = dinode->gid;

    child->parent = dinode;
    child->capacity = 4096;
    child->data = kmalloc(child->capacity);
    if (!child->data) {
        child->ref_count = 0;
        return -ENOMEM;
    }

    int r = ramfs_add_dir_entry(dinode, name, child->inum);
    if (r < 0) {
        ramfs_free_inode(child);
        return r;
    }
    *out = ramfs_make_vnode(dir->mnt, child);
    if (!*out) {
        ramfs_free_inode(child);
        return -ENOMEM;
    }
    if (*out) {
        (*out)->parent = dir;
        vnode_get(dir);
    }
    return 0;
}

static void ramfs_vnode_release(vnode_t *vn) {
    ramfs_inode_t *inode = vn ? (ramfs_inode_t *)vn->fs_data : NULL;
    vnode_put(vn->parent);
    if (inode && inode->ref_count > 1) {
        inode->ref_count--;
        ramfs_maybe_free_unlinked_inode(inode);
    }
    kfree(vn);
}

static int ramfs_vnode_unlink(vnode_t *dir, const char *name) {
    ramfs_inode_t *dinode = (ramfs_inode_t *)dir->fs_data;
    ramfs_dir_entry_t *entries = (ramfs_dir_entry_t *)dinode->data;
    int n_entries = dinode->size / sizeof(ramfs_dir_entry_t);

    for (int i = 0; i < n_entries; i++) {
        if (entries[i].name[0] != '\0' && strcmp(entries[i].name, name) == 0) {
            entries[i].name[0] = '\0';
            ramfs_inode_t *victim = ramfs_find_inode_by_inum(entries[i].inum);
            if (victim && victim->nlink > 0) {
                victim->nlink--;
                ramfs_maybe_free_unlinked_inode(victim);
            }
            return 0;
        }
    }
    return -ENOENT;
}

static int ramfs_vnode_readlink(vnode_t *vn, char *buf, size_t sz) {
    ramfs_inode_t *inode = (ramfs_inode_t *)vn->fs_data;
    if (inode->type != FT_SYMLINK) return -EINVAL;

    size_t len = inode->size;
    if (len >= sz) len = sz - 1;
    if (len > 0 && inode->data) memcpy(buf, inode->data, len);
    buf[len] = '\0';
    return (int)len;
}

static int ramfs_vnode_symlink(vnode_t *dir, const char *name, const char *target) {
    ramfs_inode_t *dinode = (ramfs_inode_t *)dir->fs_data;
    if (dinode->type != FT_DIRECTORY) return -ENOTDIR;

    ramfs_inode_t *child = ramfs_alloc_inode(FT_SYMLINK);
    if (!child) return -ENOMEM;

    child->parent = dinode;
    child->mode = S_IFLNK | 0777;
    size_t tlen = strlen(target);
    child->capacity = tlen + 1;
    child->data = kmalloc(child->capacity);
    if (!child->data) {
        child->ref_count = 0;
        return -ENOMEM;
    }

    memcpy(child->data, target, tlen + 1);
    child->size = tlen;
    int r = ramfs_add_dir_entry(dinode, name, child->inum);
    if (r < 0) {
        ramfs_free_inode(child);
        return r;
    }
    return 0;
}

static int ramfs_vnode_link(vnode_t *dir, const char *name, vnode_t *target) {
    ramfs_inode_t *dinode = (ramfs_inode_t *)dir->fs_data;
    ramfs_inode_t *inode = target ? (ramfs_inode_t *)target->fs_data : NULL;
    if (!dinode || dinode->type != FT_DIRECTORY) return -ENOTDIR;
    if (!inode) return -ENOENT;
    if (inode->type == FT_DIRECTORY) return -EPERM;
    if (ramfs_find_in_dir(dinode, name)) return -EEXIST;
    int r = ramfs_add_dir_entry(dinode, name, inode->inum);
    if (r == 0) inode->nlink++;
    return r;
}

static int ramfs_vnode_rename(vnode_t *old_dir, const char *old_name,
                              vnode_t *new_dir, const char *new_name) {
    ramfs_inode_t *old_dinode = (ramfs_inode_t *)old_dir->fs_data;
    ramfs_inode_t *new_dinode = (ramfs_inode_t *)new_dir->fs_data;
    if (old_dinode->type != FT_DIRECTORY || new_dinode->type != FT_DIRECTORY)
        return -ENOTDIR;

    ramfs_dir_entry_t *old_entries = (ramfs_dir_entry_t *)old_dinode->data;
    int n_old = old_dinode->size / sizeof(ramfs_dir_entry_t);
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

    ramfs_dir_entry_t *new_entries = (ramfs_dir_entry_t *)new_dinode->data;
    int n_new = new_dinode->size / sizeof(ramfs_dir_entry_t);
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
        ramfs_add_dir_entry(new_dinode, new_name, inum);
    }

    old_entries[old_idx].name[0] = '\0';

    ramfs_inode_t *moved = ramfs_find_inode_by_inum(inum);
    if (moved) moved->parent = new_dinode;
    return 0;
}

static int ramfs_vnode_rmdir(vnode_t *dir, const char *name) {
    ramfs_inode_t *dinode = (ramfs_inode_t *)dir->fs_data;
    ramfs_dir_entry_t *entries = (ramfs_dir_entry_t *)dinode->data;
    int n_entries = dinode->size / sizeof(ramfs_dir_entry_t);

    for (int i = 0; i < n_entries; i++) {
        if (entries[i].name[0] != '\0' && strcmp(entries[i].name, name) == 0) {
            ramfs_inode_t *child = ramfs_find_inode_by_inum(entries[i].inum);
            if (!child || child->type != FT_DIRECTORY) return -ENOTDIR;

            ramfs_dir_entry_t *centries = (ramfs_dir_entry_t *)child->data;
            int cn = child->size / sizeof(ramfs_dir_entry_t);
            int active = 0;
            for (int j = 0; j < cn; j++) {
                if (centries[j].name[0] != '\0') active++;
            }
            if (active > 2) return -ENOTEMPTY;

            entries[i].name[0] = '\0';
            if (dinode->nlink > 0)
                dinode->nlink--;
            child->nlink = 0;
            ramfs_maybe_free_unlinked_inode(child);
            return 0;
        }
    }
    return -ENOENT;
}

static int ramfs_vnode_chmod(vnode_t *vn, int mode) {
    ramfs_inode_t *inode = (ramfs_inode_t *)vn->fs_data;
    inode->mode = (inode->mode & S_IFMT) | (mode & 07777);
    vn->mode = inode->mode;
    return 0;
}

static int ramfs_vnode_chown(vnode_t *vn, int uid, int gid) {
    ramfs_inode_t *inode = (ramfs_inode_t *)vn->fs_data;
    if (uid != -1) inode->uid = (uint32_t)uid;
    if (gid != -1) inode->gid = (uint32_t)gid;
    if (uid != -1 || gid != -1) {
        if (inode->type != FT_DIRECTORY) {
            inode->mode &= ~S_ISUID;
            if (inode->mode & S_IXGRP)
                inode->mode &= ~S_ISGID;
        }
    }
    vn->uid = inode->uid;
    vn->gid = inode->gid;
    vn->mode = inode->mode;
    return 0;
}

static int ramfs_vnode_truncate(vnode_t *vn, size_t size) {
    ramfs_inode_t *inode = (ramfs_inode_t *)vn->fs_data;
    if (!inode) return -EINVAL;
    if (inode->type == FT_DIRECTORY) return -EISDIR;

    if (size < inode->capacity) {
        size_t new_cap = size ? size : 1;
        char *new_data = (char *)kmalloc(new_cap);
        if (!new_data) return -ENOMEM;
        size_t keep = inode->size < size ? inode->size : size;
        if (inode->data && keep)
            memcpy(new_data, inode->data, keep);
        if (inode->data)
            kfree(inode->data);
        inode->data = new_data;
        inode->capacity = new_cap;
    } else if (size > inode->size && inode->data && inode->size < inode->capacity) {
        size_t zero_end = size < inode->capacity ? size : inode->capacity;
        memset(inode->data + inode->size, 0, zero_end - inode->size);
    }

    inode->size = size;
    vn->size = size;
    return 0;
}

static int ramfs_vnode_writepage(vnode_t *vn, uint64_t index,
                                 const void *data, size_t len)
{
    if (!vn || !vn->fs_data || !data)
        return -EINVAL;
    ramfs_inode_t *inode = (ramfs_inode_t *)vn->fs_data;
    if (inode->type == FT_DIRECTORY)
        return -EISDIR;

    uint64_t off = index * PAGE_SIZE;
    if (off >= inode->size)
        return 0;
    size_t n = inode->size - (size_t)off;
    if (n > len)
        n = len;
    if (off + n > inode->capacity)
        return -EIO;

    memcpy(inode->data + off, data, n);
    return 0;
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
    .link = ramfs_vnode_link,
    .symlink = ramfs_vnode_symlink,
    .readlink = ramfs_vnode_readlink,
    .truncate = ramfs_vnode_truncate,
    .writepage = ramfs_vnode_writepage,
    .chmod = ramfs_vnode_chmod,
    .chown = ramfs_vnode_chown,
};

static int ramfs_fread(vfile_t *vf, char *buf, size_t count) {
    ramfs_inode_t *inode = (ramfs_inode_t *)vf->vnode->fs_data;
    if (vf->offset >= inode->size) return 0;

    size_t avail = inode->size - vf->offset;
    size_t n = count < avail ? count : avail;
    if (n > 0) {
        size_t copied = 0;
        if (vf->offset < inode->capacity && inode->data) {
            size_t in_mem = inode->capacity - vf->offset;
            if (in_mem > n) in_mem = n;
            memcpy(buf, inode->data + vf->offset, in_mem);
            copied = in_mem;
        }
        if (copied < n)
            memset(buf + copied, 0, n - copied);
        vf->offset += n;
    }
    return (int)n;
}

static int ramfs_fwrite(vfile_t *vf, const char *buf, size_t count) {
    ramfs_inode_t *inode = (ramfs_inode_t *)vf->vnode->fs_data;
    if (inode->type == FT_DIRECTORY) return -EISDIR;

    size_t needed = vf->offset + count;
    if (needed > inode->capacity) {
        size_t new_cap = needed * 2;
        char *new_data = (char *)kmalloc(new_cap);
        if (!new_data) {
            new_cap = needed;
            new_data = (char *)kmalloc(new_cap);
            if (!new_data) return -ENOMEM;
        }
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
    if (vf->offset > inode->size) {
        inode->size = vf->offset;
        vf->vnode->size = inode->size;
    }
    return (int)count;
}

static long ramfs_flseek(vfile_t *vf, long offset, int whence) {
    ramfs_inode_t *inode = (ramfs_inode_t *)vf->vnode->fs_data;
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
    ramfs_inode_t *inode = (ramfs_inode_t *)vf->vnode->fs_data;
    if (inode->type != FT_DIRECTORY) return -ENOTDIR;

    ramfs_dir_entry_t *entries = (ramfs_dir_entry_t *)inode->data;
    int n_entries = inode->size / sizeof(ramfs_dir_entry_t);
    char *out = (char *)dirp;
    size_t total = 0;

    int idx = (int)(vf->offset / sizeof(ramfs_dir_entry_t));
    while (idx < n_entries) {
        ramfs_dir_entry_t *de = &entries[idx];
        if (de->name[0] != '\0') {
            size_t namelen = strlen(de->name);
            size_t reclen = (offsetof(vfs_dirent64_t, d_name) + namelen + 1 + 7) & ~7UL;
            if (total + reclen > count) break;

            vfs_dirent64_t *d = (vfs_dirent64_t *)(out + total);
            d->d_ino = (uint64_t)de->inum;
            d->d_off = (int64_t)(total + reclen);
            d->d_reclen = (uint16_t)reclen;
            d->d_type = DT_UNKNOWN;
            ramfs_inode_t *child = ramfs_find_inode_by_inum(de->inum);
            if (child) {
                if (child->type == FT_DIRECTORY) d->d_type = DT_DIR;
                else if (child->type == FT_SYMLINK) d->d_type = DT_LNK;
                else if (child->type == FT_REGULAR) d->d_type = DT_REG;
            }
            memcpy(d->d_name, de->name, namelen + 1);
            total += reclen;
        }
        idx++;
        vf->offset += sizeof(ramfs_dir_entry_t);
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
    return ramfs_make_vnode(mnt, ramfs_get_root());
}

vnode_t *ramfs_mount_empty(mount_t *mnt) {
    ramfs_get_root();
    ramfs_inode_t *root = ramfs_alloc_inode(FT_DIRECTORY);
    if (!root) return NULL;
    root->nlink = 2;
    root->mode = S_IFDIR | 0777;
    root->capacity = RAMFS_MAX_DIR_ENTRIES * sizeof(ramfs_dir_entry_t);
    root->data = kmalloc(root->capacity);
    if (!root->data) {
        memset(root, 0, sizeof(*root));
        return NULL;
    }
    memset(root->data, 0, root->capacity);
    root->parent = root;
    ramfs_add_dir_entry(root, ".", root->inum);
    ramfs_add_dir_entry(root, "..", root->inum);
    return ramfs_make_vnode(mnt, root);
}

vfile_t *ramfs_open_vnode(vnode_t *vn, int flags) {
    vfile_t *vf = vfile_alloc();
    if (!vf) return NULL;
    vf->vnode = vn;
    vnode_get(vn);
    vf->flags = flags;
    vf->offset = (flags & O_APPEND) ? vn->size : 0;
    refcount_set(&vf->ref_count, 1);
    vf->ops = &g_ramfs_fops;
    return vf;
}
