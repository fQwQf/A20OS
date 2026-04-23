#include "fs.h"
#include "mm.h"
#include "string.h"
#include "stdio.h"
#include "proc.h"
#include "panic.h"
#include "defs.h"

#define MAX_INODES       1024  // 最大 inode 数量
#define MAX_DIR_ENTRIES   256  // 每个目录的最大条目数

// inode 表（内存文件系统）
static inode_t inode_table[MAX_INODES];
static int next_inum = 1;  // 下一个可用的 inode 编号

// 通过 inode 编号查找 inode
inode_t *fs_find_inode_by_inum(int inum) {
    for (int i = 0; i < MAX_INODES; i++)
        if (inode_table[i].ref_count > 0 && inode_table[i].inum == inum)
            return &inode_table[i];
    return NULL;
}

// 分配一个新的 inode，类型为 file 或 directory
inode_t *alloc_inode(int type) {
    for (int i = 0; i < MAX_INODES; i++) {
        if (inode_table[i].ref_count == 0) {
            inode_table[i].inum = next_inum++;
            inode_table[i].type = type;
            inode_table[i].ref_count = 1;
            inode_table[i].size = 0;
            inode_table[i].data = NULL;
            inode_table[i].capacity = 0;
            inode_table[i].parent = NULL;
            return &inode_table[i];
        }
    }
    return NULL;
}

// 获取目录的目录项数组
static dir_entry_t *get_dir_entries(inode_t *dir) {
    return (dir_entry_t *)dir->data;
}

// 获取目录中的条目数量
static int dir_entry_count(inode_t *dir) {
    return dir->size / sizeof(dir_entry_t);
}

// 在目录中添加一个目录项
int add_dir_entry(inode_t *dir, const char *name, int inum) {
    int count = dir_entry_count(dir);
    if (count >= MAX_DIR_ENTRIES) return -ENOSPC;

    size_t needed = (count + 1) * sizeof(dir_entry_t);
    if (needed > dir->capacity) {
        size_t new_cap = needed * 2;
        char *new_data = kmalloc(new_cap);
        if (!new_data) return -ENOMEM;
        if (dir->data) {
            memcpy(new_data, dir->data, dir->size);
            kfree(dir->data);
        }
        dir->data = new_data;
        dir->capacity = new_cap;
    }

    dir_entry_t *entries = get_dir_entries(dir);
    strncpy(entries[count].name, name, MAX_NAME_LEN - 1);
    entries[count].name[MAX_NAME_LEN - 1] = '\0';
    entries[count].inum = inum;
    dir->size = needed;
    return 0;
}

// 在目录中查找指定名称的 inode
static inode_t *find_in_dir(inode_t *dir, const char *name) {
    dir_entry_t *entries = get_dir_entries(dir);
    int count = dir_entry_count(dir);
    for (int i = 0; i < count; i++) {
        if (entries[i].name[0] != '\0' && strcmp(entries[i].name, name) == 0) {
            int inum = entries[i].inum;
            for (int j = 0; j < MAX_INODES; j++) {
                if (inode_table[j].inum == inum && inode_table[j].ref_count > 0)
                    return &inode_table[j];
            }
        }
    }
    return NULL;
}

// 获取根目录 inode
inode_t *fs_get_root(void) {
    return &inode_table[0];
}

// 在目录中查找指定的 inode
int fs_inode_lookup(inode_t *dir, const char *name, inode_t **out) {
    if (!dir || dir->type != FT_DIRECTORY) return -ENOTDIR;
    inode_t *found = find_in_dir(dir, name);
    if (!found) return -ENOENT;
    *out = found;
    return 0;
}

// 解析路径（处理相对路径、. 和 ..）
void fs_resolve_path(const char *path, char *resolved, size_t max_len) {
    task_t *t = proc_current();
    const char *cwd = (t && t->cwd[0]) ? t->cwd : "/";

    if (path[0] == '/') {
        strncpy(resolved, path, max_len - 1);
    } else {
        strncpy(resolved, cwd, max_len - 1);
        size_t len = strlen(resolved);
        if (len < max_len - 2) {
            resolved[len] = '/';
            strncpy(resolved + len + 1, path, max_len - len - 2);
        }
    }
    resolved[max_len - 1] = '\0';

    char parts[64][MAX_NAME_LEN];
    int depth = 0;
    char *p = resolved;
    if (*p == '/') p++;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char *end = p;
        while (*end && *end != '/') end++;
        size_t len = end - p;
        if (len == 1 && p[0] == '.') { p = end; continue; }
        if (len == 2 && p[0] == '.' && p[1] == '.') {
            if (depth > 0) depth--;
            p = end; continue;
        }
        if (depth < 64) {
            strncpy(parts[depth], p, len);
            parts[depth][len] = '\0';
            depth++;
        }
        p = end;
    }

    resolved[0] = '\0';
    if (depth == 0) { strcpy(resolved, "/"); return; }
    for (int i = 0; i < depth; i++) {
        strcat(resolved, "/");
        strcat(resolved, parts[i]);
    }
}

// 根据路径查找 inode
inode_t *fs_find_inode(const char *path) {
    if (!path) return NULL;

    char resolved[MAX_PATH_LEN];
    fs_resolve_path(path, resolved, MAX_PATH_LEN);

    if (strcmp(resolved, "/") == 0) return &inode_table[0];

    inode_t *cur = &inode_table[0];
    char buf[MAX_PATH_LEN];
    strncpy(buf, resolved, MAX_PATH_LEN - 1);
    buf[MAX_PATH_LEN - 1] = '\0';

    char *p = buf + 1;
    while (*p) {
        char *slash = strchr(p, '/');
        if (slash) *slash = '\0';

        if (strcmp(p, ".") == 0) {
            /* stay */
        } else if (strcmp(p, "..") == 0) {
            if (cur->parent) cur = cur->parent;
        } else {
            if (cur->type != FT_DIRECTORY) return NULL;
            cur = find_in_dir(cur, p);
            if (!cur) return NULL;
        }

        if (slash) p = slash + 1; else break;
    }
    return cur;
}

// 初始化文件系统
void fs_init(void) {
    memset(inode_table, 0, sizeof(inode_table));

    inode_t *root = &inode_table[0];
    root->inum = 0;
    root->type = FT_DIRECTORY;
    root->ref_count = 1;
    root->size = 0;
    root->capacity = MAX_DIR_ENTRIES * sizeof(dir_entry_t);
    root->data = kmalloc(root->capacity);
    root->parent = root;
    if (!root->data) panic("fs_init: no memory for root dir");
    memset(root->data, 0, root->capacity);

    add_dir_entry(root, ".", 0);
    add_dir_entry(root, "..", 0);

    {
        const char *text = "Hello from A20OS!\n"
            "A20 is an abbreviation of AAAAAAAAAAAAAAAAAAAAOS.\n"
            "This is a sample text file for testing.\n"
            "You can try: cat /hello.txt\n"
            "Supported commands: ls, cat, mkdir, rm, cp, pwd, cd, echo, help\n";
        size_t tlen = strlen(text);
        inode_t *f = alloc_inode(FT_REGULAR);
        if (f) {
            f->parent = root;
            f->capacity = tlen + 64;
            f->data = kmalloc(f->capacity);
            if (f->data) {
                memcpy(f->data, text, tlen);
                f->size = tlen;
            }
            add_dir_entry(root, "hello.txt", f->inum);
        }
    }

    printf("[FS] Initialized, root inode 0\n");
}

// 创建目录
int fs_mkdir(const char *path) {
    if (fs_find_inode(path)) return -EEXIST;

    char buf[MAX_PATH_LEN];
    strncpy(buf, path, MAX_PATH_LEN - 1);
    buf[MAX_PATH_LEN - 1] = '\0';
    if (buf[strlen(buf) - 1] == '/') buf[strlen(buf) - 1] = '\0';

    char *last_slash = strrchr(buf, '/');
    char *name = last_slash ? last_slash + 1 : buf;
    if (*name == '\0') return -EINVAL;

    char parent_path[MAX_PATH_LEN];
    if (last_slash == buf || last_slash == NULL) {
        strcpy(parent_path, "/");
    } else {
        int len = last_slash - buf;
        memcpy(parent_path, buf, len);
        parent_path[len] = '\0';
    }

    inode_t *parent = fs_find_inode(parent_path);
    if (!parent || parent->type != FT_DIRECTORY) return -ENOENT;

    inode_t *dir = alloc_inode(FT_DIRECTORY);
    if (!dir) return -ENOMEM;
    dir->parent = parent;
    dir->capacity = MAX_DIR_ENTRIES * sizeof(dir_entry_t);
    dir->data = kmalloc(dir->capacity);
    if (!dir->data) { dir->ref_count = 0; return -ENOMEM; }
    memset(dir->data, 0, dir->capacity);

    add_dir_entry(dir, ".", dir->inum);
    add_dir_entry(dir, "..", parent->inum);
    add_dir_entry(parent, name, dir->inum);

    return 0;
}

// 获取文件状态信息
int fs_stat(const char *path, stat_t *st) {
    inode_t *inode = fs_find_inode(path);
    if (!inode) return -ENOENT;
    st->st_ino = inode->inum;
    st->st_type = inode->type;
    st->st_size = inode->size;
    st->st_nlink = 1;
    return 0;
}

// 规范化路径（处理冗余的 /）
// 真的有必要吗？
static void path_normalize(char *path) {
    char parts[64][MAX_NAME_LEN];
    int depth = 0;
    char *p = path;
    if (*p == '/') p++;
    while (*p) {
        char *end = strchr(p, '/');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == 0 || (len == 1 && p[0] == '.')) {
            if (end) p = end + 1; else break;
            continue;
        }
        if (len == 2 && p[0] == '.' && p[1] == '.') {
            if (depth > 0) depth--;
        } else if (depth < 64) {
            memcpy(parts[depth], p, len < MAX_NAME_LEN ? len : MAX_NAME_LEN - 1);
            parts[depth][len < MAX_NAME_LEN ? len : MAX_NAME_LEN - 1] = '\0';
            depth++;
        }
        if (end) p = end + 1; else break;
    }
    path[0] = '/';
    size_t pos = 1;
    for (int i = 0; i < depth; i++) {
        size_t plen = strlen(parts[i]);
        if (pos + plen + 2 > MAX_PATH_LEN) break;
        memcpy(path + pos, parts[i], plen);
        pos += plen;
        if (i < depth - 1) path[pos++] = '/';
    }
    path[pos] = '\0';
}

// 改变当前工作目录
int fs_chdir(const char *path) {
    task_t *t = proc_current();
    if (!t) return -EINVAL;
    if (path[0] == '/') {
        strncpy(t->cwd, path, MAX_PATH_LEN - 1);
    } else {
        int len = strlen(t->cwd);
        if (len + strlen(path) + 2 > MAX_PATH_LEN) return -ENAMETOOLONG;
        if (len > 1 && t->cwd[len - 1] != '/') { t->cwd[len] = '/'; len++; }
        strcpy(t->cwd + len, path);
    }
    t->cwd[MAX_PATH_LEN - 1] = '\0';
    path_normalize(t->cwd);
    return 0;
}

// 获取当前工作目录
int fs_getcwd(char *buf, size_t size) {
    task_t *t = proc_current();
    if (!t) return -EINVAL;
    size_t len = strlen(t->cwd) + 1;
    if (size < len) return -ERANGE;
    memcpy(buf, t->cwd, len);
    return (int)len;
}
