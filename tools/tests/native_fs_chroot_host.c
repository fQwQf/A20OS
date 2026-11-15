/* Real Native path_create + syscall_path_at + Rust normalization are appended. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#define MAX_PATH_LEN 512
#define MAX_FILES 32
#define VFS_FT_DIR 1
#define A20_OK 0
#define A20_ERR_FAULT 8
#define A20_ERR_NO_ENTRY 2
#define A20_RIGHT_READ 1
#define A20_RIGHT_WRITE 2
#define A20_RIGHT_STAT 4
#define A20_RIGHT_SEEK 8
#define A20_RIGHT_DUP 16
#define A20_RIGHT_TRANSFER 32
#define A20_OBJ_DIRECTORY 2
#define A20_OBJ_FILE 1
typedef uint32_t a20_handle_t;
typedef uint32_t a20_rights_t;
typedef struct {uint64_t arg[6];} a20_syscall_args_t;
typedef struct {int type;} vnode_t;
typedef struct {vnode_t *vnode;char path[MAX_PATH_LEN];} vfile_t;
typedef struct {struct {char cwd[MAX_PATH_LEN],root_path[MAX_PATH_LEN];} fs;} task_t;
struct a20_ht_internal {int unused;};
static task_t current;
static vnode_t directory={VFS_FT_DIR};
static vfile_t capability={&directory,"/extra/cap"};
static char created[MAX_PATH_LEN];
static int creates,closed_fds,open_error;
static task_t *proc_current(void) {return &current;}
static int fdtable_get(task_t *t,int fd) {(void)t;return fd==3?3:-1;}
static vfile_t *vfs_get_file_ref(int fd) {return fd==3?&capability:NULL;}
static void vfs_put_file_ref(int fd,vfile_t *file) {(void)fd;(void)file;}
extern int a20_vfs_path_is_beneath(const char *,const char *);
extern int a20_vfs_path_normalize_absolute_with_root(char *,const char *,size_t);
static int path_is_beneath(const char *r,const char *p) {return a20_vfs_path_is_beneath(r,p);}
static int vfs_path_normalize_absolute_with_root(char *p,const char *r) {
    return a20_vfs_path_normalize_absolute_with_root(p,r,MAX_PATH_LEN);
}
static int copy_path_from_user(char *out,const char *in,uint32_t n) {
    assert(n<MAX_PATH_LEN);memcpy(out,in,n);out[n]=0;return 0;
}
static int64_t a20_dir_handle_to_dirfd(a20_handle_t h,a20_rights_t rights,int *out) {
    assert(rights==(A20_RIGHT_READ|A20_RIGHT_WRITE));
    if(h==99)return -EBADF;
    *out=h?3:AT_FDCWD;return 0;
}
static int64_t a20_native_vfs_result(int error) {return error;}
static void fdtable_close_current(int fd) {assert(fd==3||fd==7);closed_fds++;}
static int vfs_mkdir(const char *path,int mode) {
    assert(mode==0755);strcpy(created,path);creates++;return 0;
}
static int vfs_openat2(int fd,const char *path,int flags,int mode,int resolve) {
    (void)fd;(void)path;(void)flags;(void)mode;(void)resolve;
    return open_error?open_error:7;
}
static int64_t a20_install_gfd_handle(int fd,int type,a20_rights_t rights,a20_handle_t *out) {
    (void)rights;assert(fd==7&&type==A20_OBJ_DIRECTORY);*out=23;return 0;
}
static int a20_copy_struct_to_user(void *out,const void *in,size_t n) {memcpy(out,in,n);return 0;}
static struct a20_ht_internal *task_get_a20_ht(task_t *t) {(void)t;return NULL;}
static int a20_handle_remove(struct a20_ht_internal *h,a20_handle_t v) {(void)h;(void)v;return 0;}
#define A20_ARG(n) (args->arg[n])
#define A20_VALIDATE_AND_COPY(u,k) memcpy(&(k),(u),sizeof(k))
/* PATH_ARGS, shared resolver, and Native syscall are inserted here. */
