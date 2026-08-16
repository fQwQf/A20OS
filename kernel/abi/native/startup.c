/*
 * A20OS Native ABI — Task startup protocol implementation.
 * Design reference: docs/native-abi/07-startup.md §1.3–1.4
 */
#include "core/types.h"
#include "core/string.h"
#include "core/klog.h"
#include "proc/proc.h"
#include "fs/vfs.h"
#include "mm/mm.h"
#include "sys/usercopy.h"

#include "abi/native/types.h"
#include "abi/native/errno.h"
#include "abi/native/rights.h"
#include "abi/native/startup.h"
#include "abi/native/syscall_entry.h"

struct a20_ht_internal;
struct a20_ht_internal *a20_ht_create(void);
void a20_ht_destroy(struct a20_ht_internal *ht);
int64_t a20_handle_install(struct a20_ht_internal *ht, void *object,
                           uint16_t type, a20_rights_t rights);
struct a20_ht_internal *task_get_a20_ht(task_t *t);

static int64_t install_vfile_handle(struct a20_ht_internal *ht,
                                    const char *path, int flags,
                                    uint16_t expected_type,
                                    a20_rights_t rights)
{
    int gfd = vfs_open(path, flags, 0);
    if (gfd < 0) return -A20_ERR_NOT_FOUND;

    uint16_t obj_type = A20_OBJ_FILE;
    vfile_t *vf = vfs_get_file_ref(gfd);
    if (vf && vf->vnode && vf->vnode->type == VFS_FT_DIR)
        obj_type = A20_OBJ_DIRECTORY;
    if (vf) vfs_put_file_ref(gfd, vf);

    if (expected_type != A20_OBJ_INVALID && obj_type != expected_type) {
        vfs_close(gfd);
        return -A20_ERR_TYPE_MISMATCH;
    }

    int64_t h = a20_handle_install(ht, (void *)(uintptr_t)gfd, obj_type, rights);
    if (h < 0) vfs_close(gfd);
    return h;
}

int a20_prepare_start_info(task_t *task, const char *init_path,
                           uint64_t stack_top, uint64_t *out_sp)
{
    (void)init_path;
    if (!task || !out_sp) return -1;

    struct a20_ht_internal *ht = a20_ht_create();
    if (!ht) return -1;
    __atomic_store_n(&task->a20_ht, ht, __ATOMIC_RELEASE);

    a20_start_info_t info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    info.version = 1;
    info.page_size = PAGE_SIZE;

    /* a20_handle_t is uint32_t, so a failed install (negative int64) must be
     * tested on the raw return value; casting first would turn every error
     * into a positive index and silently store a bogus handle. */
    int64_t root = install_vfile_handle(
        ht, "/", O_RDONLY, A20_OBJ_DIRECTORY,
        A20_RIGHT_READ | A20_RIGHT_STAT | A20_RIGHT_DUP | A20_RIGHT_TRANSFER);
    info.root_dir = root < 0 ? A20_HANDLE_NULL : (a20_handle_t)root;

    info.cwd_dir = info.root_dir;

    int64_t std_in = install_vfile_handle(
        ht, "/dev/console", O_RDONLY, A20_OBJ_INVALID,
        A20_RIGHT_READ | A20_RIGHT_SEEK | A20_RIGHT_DUP);
    info.stdin_handle = std_in < 0 ? A20_HANDLE_NULL : (a20_handle_t)std_in;

    a20_rights_t write_rights = A20_RIGHT_WRITE | A20_RIGHT_SEEK | A20_RIGHT_DUP;
    int64_t std_out = install_vfile_handle(
        ht, "/dev/console", O_WRONLY, A20_OBJ_INVALID, write_rights);
    info.stdout_handle = std_out < 0 ? A20_HANDLE_NULL : (a20_handle_t)std_out;

    int64_t std_err = install_vfile_handle(
        ht, "/dev/console", O_WRONLY, A20_OBJ_INVALID, write_rights);
    info.stderr_handle = std_err < 0 ? A20_HANDLE_NULL : (a20_handle_t)std_err;

    a20_rights_t task_rights = A20_RIGHT_WAIT | A20_RIGHT_SIGNAL |
                               A20_RIGHT_STAT | A20_RIGHT_DUP;
    int64_t self = a20_handle_install(
        ht, (void *)(uintptr_t)task->pid, A20_OBJ_TASK, task_rights);
    info.self_task = self < 0 ? A20_HANDLE_NULL : (a20_handle_t)self;

    info.main_thread = info.self_task;

    info.default_event_queue = A20_HANDLE_NULL;

    uint64_t sp = stack_top;
    sp -= sizeof(a20_start_info_t);
    sp &= ~15ULL;

    if (copy_to_user((void *)sp, &info, sizeof(info)) < 0)
        return -1;

    *out_sp = sp;
    return 0;
}
