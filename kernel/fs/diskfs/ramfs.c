#include "fs/ramfs.h"
#include "fs/file.h"
#include "fs/rootfs_overlay.h"
#include "fs/vfs/stat_perm.h"
#include "mm/mm.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/panic.h"
#include "core/defs.h"
#include "proc/proc.h"

#define RAMFS_MAX_INODES       4096
#define RAMFS_MAX_DIR_ENTRIES   256

/* Regular files are stored as a growable array of fixed-size chunks so file
 * size is bounded only by available memory, not by the largest single
 * contiguous allocation (the buddy allocator's MAX_ORDER).  Directories,
 * symlinks and FIFOs stay in the small contiguous `data` buffer. */
#define RAMFS_CHUNK_SHIFT  12
#define RAMFS_CHUNK_SIZE   (1UL << RAMFS_CHUNK_SHIFT)

typedef struct ramfs_inode {
    int inum;
    int type;
    int ref_count;
    int nlink;
    size_t size;
    char *data;
    size_t capacity;
    char **chunks;
    size_t num_chunks;
    struct ramfs_inode *parent;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    mutex_t data_lock;
    spinlock_t fifo_lock;
    int fifo_readers;
    int fifo_writers;
} ramfs_inode_t;

typedef struct {
    int inum;
    char name[MAX_NAME_LEN];
} ramfs_dir_entry_t;

static ramfs_inode_t g_inode_table[RAMFS_MAX_INODES];
static int g_next_inum = 1;
static int g_ramfs_ready = 0;
/*
 * Directory entries and the shared inode table form one metadata graph.
 * Individual inode data_lock instances protect regular-file contents only;
 * they cannot serialize allocation or updates spanning a parent and child.
 */
static mutex_t g_ramfs_meta_lock = MUTEX_INIT;

static vnode_ops_t g_ramfs_vnode_ops;
static vfile_ops_t g_ramfs_fops;
static vfile_t *ramfs_open_vnode(vnode_t *vn, int flags);

/* Grow the chunk pointer array so at least `needed` bytes are addressable.
 * The pointer array (not the data) is what gets reallocated, so growth never
 * needs one large contiguous block. */
static int ramfs_grow_chunks(ramfs_inode_t *inode, size_t needed)
{
    size_t need_nc = (needed + RAMFS_CHUNK_SIZE - 1) >> RAMFS_CHUNK_SHIFT;
    if (need_nc <= inode->num_chunks)
        return 0;
    size_t new_nc = inode->num_chunks ? inode->num_chunks : 1;
    while (new_nc < need_nc)
        new_nc <<= 1;
    char **na = (char **)kmalloc(new_nc * sizeof(char *));
    if (!na)
        return -ENOMEM;
    if (inode->chunks) {
        memcpy(na, inode->chunks, inode->num_chunks * sizeof(char *));
        kfree(inode->chunks);
    }
    memset(na + inode->num_chunks, 0,
           (new_nc - inode->num_chunks) * sizeof(char *));
    inode->chunks = na;
    inode->num_chunks = new_nc;
    return 0;
}

/* Write len bytes at off, allocating chunks on demand.  Holes between
 * existing chunks and the write are zero-filled. */
static int ramfs_pwrite(ramfs_inode_t *inode, const char *src,
                        size_t off, size_t len)
{
    while (len > 0) {
        size_t ci = off >> RAMFS_CHUNK_SHIFT;
        size_t co = off & (RAMFS_CHUNK_SIZE - 1);
        size_t n = RAMFS_CHUNK_SIZE - co;
        if (n > len)
            n = len;
        if (!inode->chunks[ci]) {
            inode->chunks[ci] = (char *)kmalloc(RAMFS_CHUNK_SIZE);
            if (!inode->chunks[ci])
                return -ENOMEM;
            memset(inode->chunks[ci], 0, RAMFS_CHUNK_SIZE);
        }
        memcpy(inode->chunks[ci] + co, src, n);
        src += n;
        off += n;
        len -= n;
    }
    return 0;
}

/* Copy len bytes from off into dst; unallocated chunk slots read as zeros. */
static void ramfs_pread(ramfs_inode_t *inode, char *dst,
                        size_t off, size_t len)
{
    while (len > 0) {
        size_t ci = off >> RAMFS_CHUNK_SHIFT;
        size_t co = off & (RAMFS_CHUNK_SIZE - 1);
        size_t n = RAMFS_CHUNK_SIZE - co;
        if (n > len)
            n = len;
        if (ci < inode->num_chunks && inode->chunks[ci])
            memcpy(dst, inode->chunks[ci] + co, n);
        else
            memset(dst, 0, n);
        dst += n;
        off += n;
        len -= n;
    }
}

/* Zero [off, off+len) in an already-grown regular file. */
static void ramfs_pzero(ramfs_inode_t *inode, size_t off, size_t len)
{
    while (len > 0) {
        size_t ci = off >> RAMFS_CHUNK_SHIFT;
        size_t co = off & (RAMFS_CHUNK_SIZE - 1);
        size_t n = RAMFS_CHUNK_SIZE - co;
        if (n > len)
            n = len;
        if (ci < inode->num_chunks && inode->chunks[ci])
            memset(inode->chunks[ci] + co, 0, n);
        off += n;
        len -= n;
    }
}

static void ramfs_free_chunks(ramfs_inode_t *inode)
{
    if (!inode->chunks)
        return;
    for (size_t i = 0; i < inode->num_chunks; i++)
        if (inode->chunks[i])
            kfree(inode->chunks[i]);
    kfree(inode->chunks);
    inode->chunks = NULL;
    inode->num_chunks = 0;
}

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
            mutex_init(&g_inode_table[i].data_lock);
            spin_init(&g_inode_table[i].fifo_lock);
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
    ramfs_free_chunks(inode);
    if (inode->data)
        kfree(inode->data);
    memset(inode, 0, sizeof(*inode));
}

static void ramfs_maybe_free_unlinked_inode(mount_t *mnt,
                                             ramfs_inode_t *inode) {
    if (!inode || inode == &g_inode_table[0])
        return;
    if (inode->nlink == 0 && inode->ref_count <= 1) {
        vfs_drop_time_meta_identity(mnt, (uint64_t)inode->inum);
        ramfs_free_inode(inode);
    }
}

static ramfs_dir_entry_t *ramfs_dir_entries(ramfs_inode_t *dir) {
    return (ramfs_dir_entry_t *)dir->data;
}

static int ramfs_dir_entry_count(ramfs_inode_t *dir) {
    return (int)(dir->size / sizeof(ramfs_dir_entry_t));
}

static void ramfs_dir_set_entry(ramfs_inode_t *dir, const char *name, int inum) {
    ramfs_dir_entry_t *entries = ramfs_dir_entries(dir);
    int count = ramfs_dir_entry_count(dir);
    for (int i = 0; i < count; i++) {
        if (entries[i].name[0] != '\0' && strcmp(entries[i].name, name) == 0) {
            entries[i].inum = inum;
            return;
        }
    }
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

static void ramfs_remove_dir_entry_locked(ramfs_inode_t *dir,
                                          const char *name, int inum) {
    ramfs_dir_entry_t *entries = ramfs_dir_entries(dir);
    int count = ramfs_dir_entry_count(dir);
    for (int i = 0; i < count; i++) {
        if (entries[i].inum == inum && entries[i].name[0] != '\0' &&
            strcmp(entries[i].name, name) == 0) {
            entries[i].name[0] = '\0';
            return;
        }
    }
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
    mutex_init(&root->data_lock);
    spin_init(&root->fifo_lock);
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
        if (ramfs_grow_chunks(f, tlen) == 0 && ramfs_pwrite(f, text, 0, tlen) == 0)
            f->size = tlen;
        ramfs_add_dir_entry(root, "hello.txt", f->inum);
    }

    g_ramfs_ready = 1;
    printf("[RAMFS] Initialized, root inode 0\n");
}

/* g_ramfs_meta_lock must be held by the following metadata helpers. */
static ramfs_inode_t *ramfs_get_root_locked(void) {
    if (!g_ramfs_ready)
        ramfs_init_storage();
    return &g_inode_table[0];
}

static vnode_t *ramfs_make_vnode_locked(mount_t *mnt, ramfs_inode_t *inode) {
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
    mutex_lock(&g_ramfs_meta_lock);
    int r = ramfs_inode_lookup(dinode, name, &found);
    if (r < 0) {
        mutex_unlock(&g_ramfs_meta_lock);
        return r;
    }

    *out = ramfs_make_vnode_locked(dir->mnt, found);
    mutex_unlock(&g_ramfs_meta_lock);
    if (*out) {
        (*out)->parent = dir;
        vnode_get(dir);
    }
    return (*out) ? 0 : -ENOMEM;
}

static int ramfs_vnode_stat(vnode_t *vn, kstat_t *st) {
    ramfs_inode_t *inode = (ramfs_inode_t *)vn->fs_data;
    mutex_lock(&g_ramfs_meta_lock);
    memset(st, 0, sizeof(*st));
    st->st_ino = inode->inum;
    st->st_mode = inode->mode;
    st->st_uid = inode->uid;
    st->st_gid = inode->gid;
    st->st_size = inode->size;
    st->st_nlink = inode->nlink > 0 ? (uint32_t)inode->nlink : 1;
    mutex_unlock(&g_ramfs_meta_lock);
    return 0;
}

static int ramfs_vnode_mkdir_locked(vnode_t *dir, const char *name, int mode) {
    ramfs_inode_t *dinode = (ramfs_inode_t *)dir->fs_data;
    if (!dinode || dinode->type != FT_DIRECTORY)
        return -ENOTDIR;
    if (ramfs_find_in_dir(dinode, name))
        return -EEXIST;
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

static int ramfs_vnode_mkdir(vnode_t *dir, const char *name, int mode) {
    mutex_lock(&g_ramfs_meta_lock);
    int r = ramfs_vnode_mkdir_locked(dir, name, mode);
    mutex_unlock(&g_ramfs_meta_lock);
    return r;
}

static int ramfs_vnode_create_locked(vnode_t *dir, const char *name, int mode,
                                     vnode_t **out) {
    ramfs_inode_t *dinode = (ramfs_inode_t *)dir->fs_data;
    if (!dinode || dinode->type != FT_DIRECTORY)
        return -ENOTDIR;
    if (ramfs_find_in_dir(dinode, name))
        return -EEXIST;
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
    /* Regular files allocate chunk storage lazily on first write, so file
     * size is not limited by the largest single contiguous allocation. */
    child->capacity = 0;
    child->data = NULL;
    child->chunks = NULL;
    child->num_chunks = 0;

    int r = ramfs_add_dir_entry(dinode, name, child->inum);
    if (r < 0) {
        ramfs_free_inode(child);
        return r;
    }
    *out = ramfs_make_vnode_locked(dir->mnt, child);
    if (!*out) {
        ramfs_remove_dir_entry_locked(dinode, name, child->inum);
        ramfs_free_inode(child);
        return -ENOMEM;
    }
    if (*out) {
        (*out)->parent = dir;
        vnode_get(dir);
    }
    return 0;
}

static int ramfs_vnode_create(vnode_t *dir, const char *name, int mode,
                              vnode_t **out) {
    mutex_lock(&g_ramfs_meta_lock);
    int r = ramfs_vnode_create_locked(dir, name, mode, out);
    mutex_unlock(&g_ramfs_meta_lock);
    return r;
}

static void ramfs_vnode_release(vnode_t *vn) {
    ramfs_inode_t *inode = vn ? (ramfs_inode_t *)vn->fs_data : NULL;
    vnode_put(vn->parent);
    mutex_lock(&g_ramfs_meta_lock);
    if (inode && inode->ref_count > 1) {
        inode->ref_count--;
        ramfs_maybe_free_unlinked_inode(vn->mnt, inode);
    }
    mutex_unlock(&g_ramfs_meta_lock);
    kfree(vn);
}

static int ramfs_vnode_unlink_locked(vnode_t *dir, const char *name) {
    ramfs_inode_t *dinode = (ramfs_inode_t *)dir->fs_data;
    ramfs_dir_entry_t *entries = (ramfs_dir_entry_t *)dinode->data;
    int n_entries = dinode->size / sizeof(ramfs_dir_entry_t);

    for (int i = 0; i < n_entries; i++) {
        if (entries[i].name[0] != '\0' && strcmp(entries[i].name, name) == 0) {
            entries[i].name[0] = '\0';
            ramfs_inode_t *victim = ramfs_find_inode_by_inum(entries[i].inum);
            if (victim && victim->nlink > 0) {
                victim->nlink--;
                ramfs_maybe_free_unlinked_inode(dir->mnt, victim);
            }
            return 0;
        }
    }
    return -ENOENT;
}

static int ramfs_vnode_unlink(vnode_t *dir, const char *name) {
    mutex_lock(&g_ramfs_meta_lock);
    int r = ramfs_vnode_unlink_locked(dir, name);
    mutex_unlock(&g_ramfs_meta_lock);
    return r;
}

static int ramfs_vnode_readlink(vnode_t *vn, char *buf, size_t sz) {
    ramfs_inode_t *inode = (ramfs_inode_t *)vn->fs_data;
    mutex_lock(&g_ramfs_meta_lock);
    if (inode->type != FT_SYMLINK) {
        mutex_unlock(&g_ramfs_meta_lock);
        return -EINVAL;
    }

    size_t len = inode->size;
    if (len >= sz) len = sz - 1;
    if (len > 0 && inode->data) memcpy(buf, inode->data, len);
    buf[len] = '\0';
    mutex_unlock(&g_ramfs_meta_lock);
    return (int)len;
}

static int ramfs_vnode_symlink_locked(vnode_t *dir, const char *name,
                                      const char *target) {
    ramfs_inode_t *dinode = (ramfs_inode_t *)dir->fs_data;
    if (dinode->type != FT_DIRECTORY) return -ENOTDIR;
    if (ramfs_find_in_dir(dinode, name)) return -EEXIST;

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

static int ramfs_vnode_symlink(vnode_t *dir, const char *name,
                               const char *target) {
    mutex_lock(&g_ramfs_meta_lock);
    int r = ramfs_vnode_symlink_locked(dir, name, target);
    mutex_unlock(&g_ramfs_meta_lock);
    return r;
}

static int ramfs_vnode_link_locked(vnode_t *dir, const char *name,
                                   vnode_t *target) {
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

static int ramfs_vnode_link(vnode_t *dir, const char *name, vnode_t *target) {
    mutex_lock(&g_ramfs_meta_lock);
    int r = ramfs_vnode_link_locked(dir, name, target);
    mutex_unlock(&g_ramfs_meta_lock);
    return r;
}

static int ramfs_vnode_rename_locked(vnode_t *old_dir, const char *old_name,
                                     vnode_t *new_dir, const char *new_name,
                                     unsigned int flags) {
    ramfs_inode_t *old_dinode = (ramfs_inode_t *)old_dir->fs_data;
    ramfs_inode_t *new_dinode = (ramfs_inode_t *)new_dir->fs_data;
    if (old_dinode->type != FT_DIRECTORY || new_dinode->type != FT_DIRECTORY)
        return -ENOTDIR;
    if (flags & ~(RENAME_NOREPLACE | RENAME_EXCHANGE))
        return -EINVAL;

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
    int new_inum = 0;
    for (int i = 0; i < n_new; i++) {
        if (new_entries[i].name[0] != '\0' && strcmp(new_entries[i].name, new_name) == 0) {
            new_idx = i;
            new_inum = new_entries[i].inum;
            break;
        }
    }

    if (flags & RENAME_NOREPLACE) {
        if (new_idx >= 0) return -EEXIST;
    }

    if (flags & RENAME_EXCHANGE) {
        if (new_idx < 0) return -ENOENT;
        new_entries[new_idx].inum = inum;
        old_entries[old_idx].inum = new_inum;
        ramfs_inode_t *moved = ramfs_find_inode_by_inum(inum);
        ramfs_inode_t *other = ramfs_find_inode_by_inum(new_inum);
        if (moved && moved->type == FT_DIRECTORY) {
            moved->parent = new_dinode;
            ramfs_dir_set_entry(moved, "..", new_dinode->inum);
        } else if (moved) {
            moved->parent = new_dinode;
        }
        if (other && other->type == FT_DIRECTORY) {
            other->parent = old_dinode;
            ramfs_dir_set_entry(other, "..", old_dinode->inum);
        } else if (other) {
            other->parent = old_dinode;
        }
        return 0;
    }

    ramfs_inode_t *moved = ramfs_find_inode_by_inum(inum);
    ramfs_inode_t *victim = new_idx >= 0 ? ramfs_find_inode_by_inum(new_inum) : NULL;

    if (new_idx >= 0 && new_inum == inum)
        return 0;
    if (!moved)
        return -ENOENT;
    if (victim) {
        if (moved->type == FT_DIRECTORY && victim->type != FT_DIRECTORY)
            return -ENOTDIR;
        if (moved->type != FT_DIRECTORY && victim->type == FT_DIRECTORY)
            return -EISDIR;
        if (victim->type == FT_DIRECTORY) {
            ramfs_dir_entry_t *ventries = ramfs_dir_entries(victim);
            int vn = ramfs_dir_entry_count(victim);
            int active = 0;
            for (int i = 0; i < vn; i++) {
                if (ventries[i].name[0] != '\0')
                    active++;
            }
            if (active > 2)
                return -ENOTEMPTY;
        }
    }

    if (new_idx >= 0) {
        new_entries[new_idx].inum = inum;
        memcpy(new_entries[new_idx].name, new_name, MAX_NAME_LEN);
    } else {
        int r = ramfs_add_dir_entry(new_dinode, new_name, inum);
        if (r < 0)
            return r;
    }

    old_entries[old_idx].name[0] = '\0';

    if (victim) {
        if (victim->type == FT_DIRECTORY && new_dinode->nlink > 0)
            new_dinode->nlink--;
        if (victim->nlink > 0)
            victim->nlink--;
        ramfs_maybe_free_unlinked_inode(new_dir->mnt, victim);
    }

    if (moved->type == FT_DIRECTORY) {
        if (old_dinode != new_dinode) {
            if (old_dinode->nlink > 0)
                old_dinode->nlink--;
            new_dinode->nlink++;
        }
        moved->parent = new_dinode;
        ramfs_dir_set_entry(moved, "..", new_dinode->inum);
    } else {
        moved->parent = new_dinode;
    }
    return 0;
}

static int ramfs_vnode_rename(vnode_t *old_dir, const char *old_name,
                              vnode_t *new_dir, const char *new_name,
                              unsigned int flags) {
    mutex_lock(&g_ramfs_meta_lock);
    int r = ramfs_vnode_rename_locked(old_dir, old_name, new_dir, new_name,
                                      flags);
    mutex_unlock(&g_ramfs_meta_lock);
    return r;
}

static int ramfs_vnode_rmdir_locked(vnode_t *dir, const char *name) {
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
            ramfs_maybe_free_unlinked_inode(dir->mnt, child);
            return 0;
        }
    }
    return -ENOENT;
}

static int ramfs_vnode_rmdir(vnode_t *dir, const char *name) {
    mutex_lock(&g_ramfs_meta_lock);
    int r = ramfs_vnode_rmdir_locked(dir, name);
    mutex_unlock(&g_ramfs_meta_lock);
    return r;
}

static int ramfs_vnode_chmod(vnode_t *vn, int mode) {
    ramfs_inode_t *inode = (ramfs_inode_t *)vn->fs_data;
    mutex_lock(&g_ramfs_meta_lock);
    inode->mode = (inode->mode & S_IFMT) | (mode & 07777);
    vn->mode = inode->mode;
    mutex_unlock(&g_ramfs_meta_lock);
    return 0;
}

static int ramfs_vnode_chown(vnode_t *vn, int uid, int gid) {
    ramfs_inode_t *inode = (ramfs_inode_t *)vn->fs_data;
    mutex_lock(&g_ramfs_meta_lock);
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
    mutex_unlock(&g_ramfs_meta_lock);
    return 0;
}

static int ramfs_vnode_truncate(vnode_t *vn, size_t size) {
    ramfs_inode_t *inode = (ramfs_inode_t *)vn->fs_data;
    if (!inode) return -EINVAL;
    if (inode->type == FT_DIRECTORY) return -EISDIR;

    mutex_lock(&inode->data_lock);

    if (inode->type == FT_REGULAR) {
        if (size > inode->size) {
            int r = ramfs_grow_chunks(inode, size);
            if (r < 0) {
                mutex_unlock(&inode->data_lock);
                return r;
            }
            ramfs_pzero(inode, inode->size, size - inode->size);
        } else if (size < inode->size) {
            size_t keep_nc = (size + RAMFS_CHUNK_SIZE - 1) >>
                             RAMFS_CHUNK_SHIFT;
            for (size_t i = keep_nc; i < inode->num_chunks; i++) {
                if (inode->chunks[i]) {
                    kfree(inode->chunks[i]);
                    inode->chunks[i] = NULL;
                }
            }
        }
    } else if (size > inode->capacity) {
        size_t new_cap = size * 2;
        char *new_data = (char *)kmalloc(new_cap);
        if (!new_data) {
            new_cap = size;
            new_data = (char *)kmalloc(new_cap);
            if (!new_data) {
                mutex_unlock(&inode->data_lock);
                return -ENOMEM;
            }
        }
        if (inode->data) {
            size_t copy_len = inode->size < inode->capacity ? inode->size : inode->capacity;
            if (copy_len > 0) memcpy(new_data, inode->data, copy_len);
            kfree(inode->data);
        }
        size_t zero_start = inode->size < inode->capacity ? inode->size : inode->capacity;
        if (new_cap > zero_start) {
            memset(new_data + zero_start, 0, new_cap - zero_start);
        }
        inode->data = new_data;
        inode->capacity = new_cap;
    } else if (size < inode->capacity) {
        size_t new_cap = size ? size : 1;
        char *new_data = (char *)kmalloc(new_cap);
        if (!new_data) {
            mutex_unlock(&inode->data_lock);
            return -ENOMEM;
        }
        size_t keep = inode->size < size ? inode->size : size;
        if (keep > inode->capacity) keep = inode->capacity;
        if (inode->data && keep > 0)
            memcpy(new_data, inode->data, keep);
        if (inode->data)
            kfree(inode->data);
        inode->data = new_data;
        inode->capacity = new_cap;
    } else if (size > inode->size && inode->data && inode->size < inode->capacity) {
        size_t zero_end = size < inode->capacity ? size : inode->capacity;
        if (zero_end > inode->size) {
            memset(inode->data + inode->size, 0, zero_end - inode->size);
        }
    }

    inode->size = size;
    vn->size = size;
    mutex_unlock(&inode->data_lock);
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

    mutex_lock(&inode->data_lock);
    uint64_t off = index * PAGE_SIZE;
    if (off >= inode->size) {
        mutex_unlock(&inode->data_lock);
        return 0;
    }
    size_t n = inode->size - (size_t)off;
    if (n > len)
        n = len;
    if (inode->type == FT_REGULAR) {
        int r = ramfs_grow_chunks(inode, off + n);
        if (r == 0)
            r = ramfs_pwrite(inode, data, off, n);
        mutex_unlock(&inode->data_lock);
        return r;
    }
    if (off + n > inode->capacity) {
        mutex_unlock(&inode->data_lock);
        return -EIO;
    }

    memcpy(inode->data + off, data, n);
    mutex_unlock(&inode->data_lock);
    return 0;
}

static int ramfs_vnode_readpage(vnode_t *vn, uint64_t index,
                                void *data, size_t len)
{
    if (!vn || !vn->fs_data || !data)
        return -EINVAL;
    ramfs_inode_t *inode = (ramfs_inode_t *)vn->fs_data;
    if (inode->type == FT_DIRECTORY)
        return -EISDIR;
    mutex_lock(&inode->data_lock);
    memset(data, 0, len);
    uint64_t off = index * PAGE_SIZE;
    if (off >= inode->size) {
        mutex_unlock(&inode->data_lock);
        return 0;
    }
    size_t n = inode->size - (size_t)off;
    if (n > len)
        n = len;
    if (inode->type == FT_REGULAR) {
        ramfs_pread(inode, data, off, n);
        mutex_unlock(&inode->data_lock);
        return (int)n;
    }
    if (off + n > inode->capacity || !inode->data) {
        mutex_unlock(&inode->data_lock);
        return (int)n;
    }
    memcpy(data, inode->data + off, n);
    mutex_unlock(&inode->data_lock);
    return (int)n;
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
    .readpage = ramfs_vnode_readpage,
    .writepage = ramfs_vnode_writepage,
    .chmod = ramfs_vnode_chmod,
    .chown = ramfs_vnode_chown,
    .open = ramfs_open_vnode,
};

static int ramfs_fread(vfile_t *vf, char *buf, size_t count) {
    ramfs_inode_t *inode = (ramfs_inode_t *)vf->vnode->fs_data;
    mutex_lock(&inode->data_lock);
    if (vf->offset >= inode->size) {
        if ((inode->mode & S_IFMT) == S_IFIFO) {
            spin_lock(&inode->fifo_lock);
            int writers = inode->fifo_writers;
            spin_unlock(&inode->fifo_lock);
            if (writers > 0 && (vf->flags & O_NONBLOCK)) {
                mutex_unlock(&inode->data_lock);
                return -EAGAIN;
            }
        }
        mutex_unlock(&inode->data_lock);
        return 0;
    }

    size_t avail = inode->size - vf->offset;
    size_t n = count < avail ? count : avail;
    if (n > 0) {
        if (inode->type == FT_REGULAR) {
            ramfs_pread(inode, buf, vf->offset, n);
        } else {
            size_t copied = 0;
            if (vf->offset < inode->capacity && inode->data) {
                size_t in_mem = inode->capacity - vf->offset;
                if (in_mem > n) in_mem = n;
                memcpy(buf, inode->data + vf->offset, in_mem);
                copied = in_mem;
            }
            if (copied < n)
                memset(buf + copied, 0, n - copied);
        }
        vf->offset += n;
    }
    mutex_unlock(&inode->data_lock);
    return (int)n;
}

static int ramfs_fwrite(vfile_t *vf, const char *buf, size_t count) {
    ramfs_inode_t *inode = (ramfs_inode_t *)vf->vnode->fs_data;
    if (inode->type == FT_DIRECTORY) return -EISDIR;

    mutex_lock(&inode->data_lock);

    size_t needed = vf->offset + count;
    if (inode->type == FT_REGULAR) {
        int r = ramfs_grow_chunks(inode, needed);
        if (r == 0)
            r = ramfs_pwrite(inode, buf, vf->offset, count);
        if (r < 0) {
            mutex_unlock(&inode->data_lock);
            return r;
        }
    } else {
        if (needed > inode->capacity) {
            size_t new_cap = needed * 2;
            char *new_data = (char *)kmalloc(new_cap);
            if (!new_data) {
                new_cap = needed;
                new_data = (char *)kmalloc(new_cap);
                if (!new_data) {
                    mutex_unlock(&inode->data_lock);
                    return -ENOMEM;
                }
            }
            if (inode->data) {
                size_t copy_len = inode->size < inode->capacity ? inode->size : inode->capacity;
                if (copy_len > 0) memcpy(new_data, inode->data, copy_len);
                kfree(inode->data);
            }
            size_t zero_start = inode->size < inode->capacity ? inode->size : inode->capacity;
            if (new_cap > zero_start) {
                memset(new_data + zero_start, 0, new_cap - zero_start);
            }
            inode->data = new_data;
            inode->capacity = new_cap;
        }
        memcpy(inode->data + vf->offset, buf, count);
    }
    vf->offset += count;
    if (vf->offset > inode->size) {
        inode->size = vf->offset;
        vf->vnode->size = inode->size;
    }
    mutex_unlock(&inode->data_lock);
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

    mutex_lock(&g_ramfs_meta_lock);
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
    mutex_unlock(&g_ramfs_meta_lock);
    return (int)total;
}

static int ramfs_fclose(vfile_t *vf) {
    ramfs_inode_t *inode = vf && vf->vnode ?
                           (ramfs_inode_t *)vf->vnode->fs_data : NULL;
    if (inode && (inode->mode & S_IFMT) == S_IFIFO) {
        int access = vf->flags & O_ACCMODE;
        spin_lock(&inode->fifo_lock);
        if ((access == O_RDONLY || access == O_RDWR) && inode->fifo_readers > 0)
            inode->fifo_readers--;
        if ((access == O_WRONLY || access == O_RDWR) && inode->fifo_writers > 0)
            inode->fifo_writers--;
        spin_unlock(&inode->fifo_lock);
    }
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
    mutex_lock(&g_ramfs_meta_lock);
    vnode_t *root = ramfs_make_vnode_locked(mnt, ramfs_get_root_locked());
    mutex_unlock(&g_ramfs_meta_lock);
    return root;
}

vnode_t *ramfs_mount_empty(mount_t *mnt) {
    mutex_lock(&g_ramfs_meta_lock);
    ramfs_get_root_locked();
    ramfs_inode_t *root = ramfs_alloc_inode(FT_DIRECTORY);
    if (!root) {
        mutex_unlock(&g_ramfs_meta_lock);
        return NULL;
    }
    root->nlink = 2;
    root->mode = S_IFDIR | 0777;
    root->capacity = RAMFS_MAX_DIR_ENTRIES * sizeof(ramfs_dir_entry_t);
    root->data = kmalloc(root->capacity);
    if (!root->data) {
        memset(root, 0, sizeof(*root));
        mutex_unlock(&g_ramfs_meta_lock);
        return NULL;
    }
    memset(root->data, 0, root->capacity);
    root->parent = root;
    ramfs_add_dir_entry(root, ".", root->inum);
    ramfs_add_dir_entry(root, "..", root->inum);
    vnode_t *vn = ramfs_make_vnode_locked(mnt, root);
    if (!vn)
        ramfs_free_inode(root);
    mutex_unlock(&g_ramfs_meta_lock);
    return vn;
}

static vfile_t *ramfs_open_vnode(vnode_t *vn, int flags) {
    vfile_t *vf = vfile_alloc();
    if (!vf) return NULL;
    vf->vnode = vn;
    vnode_get(vn);
    vf->flags = flags;
    vf->offset = (flags & O_APPEND) ? vn->size : 0;
    vfile_ref_init(vf, 1);
    vf->ops = &g_ramfs_fops;
    ramfs_inode_t *inode = (ramfs_inode_t *)vn->fs_data;
    if (inode && (inode->mode & S_IFMT) == S_IFIFO) {
        int access = flags & O_ACCMODE;
        spin_lock(&inode->fifo_lock);
        if (access == O_RDONLY || access == O_RDWR)
            inode->fifo_readers++;
        if (access == O_WRONLY || access == O_RDWR)
            inode->fifo_writers++;
        spin_unlock(&inode->fifo_lock);
    }
    return vf;
}

/* Populate the root ramfs from the build-time overlay records.
 * Returns 0 on success or the first encountered negative errno. */
static int ramfs_populate_entries(const rootfs_overlay_entry_t *entries, size_t count) {
    for (size_t i = 0; i < count; i++) {
        const rootfs_overlay_entry_t *e = &entries[i];
        char path[MAX_PATH_LEN];

        strncpy(path, e->path, MAX_PATH_LEN - 1);
        path[MAX_PATH_LEN - 1] = '\0';

        /* Create every parent directory along the absolute path. */
        char *p = path;
        while ((p = strchr(p + 1, '/')) != NULL) {
            *p = '\0';
            int r = vfs_mkdir(path, 0755);
            if (r < 0 && r != -EEXIST)
                return r;
            *p = '/';
        }

        int fd = vfs_open(e->path, O_CREAT | O_WRONLY | O_TRUNC, e->mode);
        if (fd < 0)
            return fd;

        int wn = vfs_write(fd, e->content, e->size);
        vfs_close(fd);
        if (wn < 0)
            return wn;
        if ((size_t)wn != e->size)
            return -EIO;
    }
    return 0;
}

int ramfs_populate_overlay(void) {
    rootfs_driver_overlay_init();
    int r = ramfs_populate_entries(g_rootfs_overlay, g_rootfs_overlay_count);
    if (r < 0)
        return r;
    return ramfs_populate_entries(g_rootfs_driver_overlay,
                                  g_rootfs_driver_overlay_count);
}
