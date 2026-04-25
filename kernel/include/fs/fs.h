#ifndef _FS_H
#define _FS_H

#include "core/types.h"
#include "core/consts.h"

/* In-memory inode */
typedef struct inode {
    int inum;
    int type;
    int ref_count;
    size_t size;
    char *data;
    size_t capacity;
    struct inode *parent;
} inode_t;

/* Directory entry (stored in directory inode's data buffer) */
typedef struct {
    int inum;
    char name[MAX_NAME_LEN];
} dir_entry_t;

typedef struct {
    int st_dev;
    int st_ino;
    int st_mode;
    int st_nlink;
    size_t st_size;
    int st_type;
} stat_t;

/* Init */
void fs_init(void);

/* Directory */
int  fs_mkdir(const char *path);

int  fs_stat(const char *path, stat_t *st);

/* Path / cwd */
int  fs_chdir(const char *path);
int  fs_getcwd(char *buf, size_t size);

/* Inode lookup */
inode_t *fs_find_inode(const char *path);
inode_t *fs_get_root(void);
int      fs_inode_lookup(inode_t *dir, const char *name, inode_t **out);
inode_t *fs_find_inode_by_inum(int inum);
void fs_resolve_path(const char *path, char *resolved, size_t max_len);
inode_t *alloc_inode(int type);
int add_dir_entry(inode_t *dir, const char *name, int inum);

#endif /* _FS_H */
