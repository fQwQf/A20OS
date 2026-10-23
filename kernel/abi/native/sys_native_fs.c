/*
 * A20OS Native ABI — Phase 2 syscall implementations.
 *
 * This file is part of the mechanically split Native Phase 2 ABI.
 * See sys_phase2.c for shared helpers and forward declarations.
 */
#include "core/types.h"
#include <limits.h>
#include "core/defs.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/consts.h"
#include "core/version.h"
#include "core/timekeeping.h"
#include "core/timer.h"
#include "core/random.h"
#include "trap_frame.h"
#include "proc/proc.h"
#include "mm/mm.h"
#include "mm/slab.h"
#include "mm/frame.h"
#include "mm/vm.h"
#include "fs/vfs.h"
#include "fs/uxfs.h"
#include "fs/vfs/path.h"
#include "fs/fdtable.h"
#include "fs/tty.h"
#include "fs/locks.h"
#include "fs/xattr.h"
#include "net/socket.h"
#include "sys/usercopy.h"

#include "abi/native/types.h"
#include "abi/native/errno.h"
#include "abi/native/rights.h"
#include "core/fcntl.h"
#include "abi/native/syscall_entry.h"
#include "sys_validate.h"
#include "abi/native/startup.h"
#include "abi/native/vmo.h"
#include "abi/native/vmar.h"
#include "abi/native/ipc_internal.h"
#include "abi/native/resource.h"

#define A20_ARG(n) (args->arg[(n)])

extern struct a20_ht_internal *task_get_a20_ht(task_t *t);
extern int64_t a20_handle_install(struct a20_ht_internal *ht, void *object,
                                  uint16_t type, a20_rights_t rights);
extern int64_t a20_handle_install_temporal(struct a20_ht_internal *ht, void *object,
                                           uint16_t type, a20_rights_t rights,
                                           uint64_t expiry_tick, uint32_t remaining_ops,
                                           uint32_t temporal_flags, uint8_t security_label);
extern int64_t a20_handle_lookup_internal(struct a20_ht_internal *ht, a20_handle_t h,
                                           uint16_t expected_type, a20_rights_t required_rights,
                                           a20_handle_entry_t *out);
extern int64_t a20_handle_lookup_ref_internal(struct a20_ht_internal *ht,
                                               a20_handle_t h,
                                               uint16_t expected_type,
                                               a20_rights_t required_rights,
                                               a20_handle_entry_t *out);
extern int64_t a20_handle_remove(struct a20_ht_internal *ht, a20_handle_t h);
extern void a20_object_release(void *object, uint16_t type);

extern uint8_t a20_ht_get_label(struct a20_ht_internal *ht);
extern void a20_ht_set_label(struct a20_ht_internal *ht, uint8_t label);
extern int64_t a20_handle_set_temporal(struct a20_ht_internal *ht, a20_handle_t h,
                                       uint64_t expiry_tick, uint32_t remaining_ops,
                                       uint32_t temporal_flags);
extern int64_t a20_handle_get_temporal(struct a20_ht_internal *ht, a20_handle_t h,
                                       uint64_t *expiry_tick, uint32_t *remaining_ops,
                                       uint32_t *temporal_flags);
extern int64_t a20_handle_set_label(struct a20_ht_internal *ht, a20_handle_t h,
                                    uint8_t label);

extern int copy_path_from_user(char *dst, const char *uptr, uint32_t len);
extern void resolve_path(const char *in, char *out);
extern int64_t a20_path_open_impl(const a20_path_open_args_t *kargs,
                                   a20_handle_t *out_handle);
extern int64_t a20_dir_handle_to_dirfd(a20_handle_t dir_h, a20_rights_t required_rights,
                                        int *out_dirfd);
extern int64_t a20_install_gfd_handle(int gfd, uint16_t obj_type_hint,
                                       a20_rights_t requested_rights,
                                       a20_handle_t *out_handle);

int64_t a20_native_vfs_result(int r)
{
    if (r >= 0) return r;
    switch (-r) {
    case EPERM:        return -A20_ERR_PERM;
    case EACCES:       return -A20_ERR_ACCESS;
    case ENOENT:       return -A20_ERR_NO_ENTRY;
    case EBADF:        return -A20_ERR_BAD_HANDLE;
    case ENOMEM:       return -A20_ERR_NO_MEMORY;
    case EEXIST:       return -A20_ERR_EXISTS;
    case ENOTDIR:      return -A20_ERR_NOT_DIR;
    case EISDIR:       return -A20_ERR_IS_DIR;
    case ENOTEMPTY:    return -A20_ERR_NOT_EMPTY;
    case ENAMETOOLONG: return -A20_ERR_NAME_TOO_LONG;
    case ENOSPC:       return -A20_ERR_NO_SPACE;
    case EDQUOT:       return -A20_ERR_NO_SPACE;
    case EROFS:        return -A20_ERR_ACCESS;
    case EAGAIN:       return -A20_ERR_WOULD_BLOCK;
    case EINTR:
    case ERESTARTSYS:  return -A20_ERR_INTERRUPTED;
    case EFAULT:       return -A20_ERR_FAULT;
    case EOVERFLOW:    return -A20_ERR_OVERFLOW;
    case ENOLCK:       return -A20_ERR_NO_LOCKS;
    case EDEADLK:      return -A20_ERR_DEADLOCK;
    case EPIPE:        return -A20_ERR_BROKEN_PIPE;
    case ESPIPE:       return -A20_ERR_ILLEGAL_SEEK;
    case ENOTTY:       return -A20_ERR_NOT_TERMINAL;
    case ETIMEDOUT:    return -A20_ERR_TIMED_OUT;
    case EINVAL:       return -A20_ERR_INVALID_ARGUMENT;
    case ENOSYS:       return -A20_ERR_NOT_SUPPORTED;
    case EOPNOTSUPP:   return -A20_ERR_NOT_SUPPORTED;
    default:           return -A20_ERR_IO;
    }
}

/* Resolve a relative path against a directory capability.  The older Native
 * path syscalls resolve against the task cwd; the *_at extension keeps the
 * POSIX dirfd contract without exposing kernel fd numbers to userspace. */
static int64_t a20_native_join_dir_path(a20_handle_t dir_h, const char *path,
                                         char *out)
{
    if (!path || !out)
        return -A20_ERR_INVALID_ARGUMENT;
    if (path[0] == '/') {
        if (strlen(path) >= MAX_PATH_LEN)
            return -A20_ERR_NAME_TOO_LONG;
        strncpy(out, path, MAX_PATH_LEN - 1);
        out[MAX_PATH_LEN - 1] = '\0';
        return A20_OK;
    }

    int dirfd;
    int64_t r = a20_dir_handle_to_dirfd(dir_h, A20_RIGHT_READ, &dirfd);
    if (r < 0)
        return r;
    char base[MAX_PATH_LEN];
    int vr = vfs_dirfd_path(dirfd, base, sizeof(base));
    if (dirfd != AT_FDCWD)
        fdtable_close_current(dirfd);
    if (vr < 0)
        return a20_native_vfs_result(vr);
    vr = vfs_path_join(base, path, out, MAX_PATH_LEN);
    if (vr < 0 || vfs_path_normalize_absolute(out) < 0)
        return -A20_ERR_NAME_TOO_LONG;
    return A20_OK;
}

static int64_t a20_native_copy_join(a20_handle_t dir_h, uint64_t upath,
                                    uint32_t path_len, char *out)
{
    char path[MAX_PATH_LEN];
    if (copy_path_from_user(path, (const char *)(uintptr_t)upath, path_len) < 0)
        return -A20_ERR_FAULT;
    return a20_native_join_dir_path(dir_h, path, out);
}

/* ===== Path/Filesystem (0x0400) continued ===== */

static const uint32_t terminal_baud_rates[] = {0,50,75,110,134,150,200,300,600,1200,1800,2400,4800,9600,19200,38400,57600,115200,230400,460800,500000,576000,921600,1000000,1152000,1500000,2000000,2500000,3000000,3500000,4000000};
static int terminal_to_native(a20_terminal_config_t *native, const ktty_termios_t *state) {
    if (state->c_iflag & ~32767u) return -1;
    native->input_modes = 0;
    if (state->c_iflag & 256u) native->input_modes |= 1u << 0;
    if (state->c_iflag & 64u) native->input_modes |= 1u << 1;
    if (state->c_iflag & 128u) native->input_modes |= 1u << 2;
    if (state->c_iflag & 1u) native->input_modes |= 1u << 3;
    if (state->c_iflag & 2u) native->input_modes |= 1u << 4;
    if (state->c_iflag & 4u) native->input_modes |= 1u << 5;
    if (state->c_iflag & 8u) native->input_modes |= 1u << 6;
    if (state->c_iflag & 16u) native->input_modes |= 1u << 7;
    if (state->c_iflag & 32u) native->input_modes |= 1u << 8;
    if (state->c_iflag & 512u) native->input_modes |= 1u << 9;
    if (state->c_iflag & 1024u) native->input_modes |= 1u << 10;
    if (state->c_iflag & 2048u) native->input_modes |= 1u << 11;
    if (state->c_iflag & 4096u) native->input_modes |= 1u << 12;
    if (state->c_iflag & 8192u) native->input_modes |= 1u << 13;
    if (state->c_iflag & 16384u) native->input_modes |= 1u << 14;
    if (state->c_oflag & ~255u) return -1;
    native->output_modes = 0;
    if (state->c_oflag & 1u) native->output_modes |= 1u << 0;
    if (state->c_oflag & 4u) native->output_modes |= 1u << 1;
    if (state->c_oflag & 8u) native->output_modes |= 1u << 2;
    if (state->c_oflag & 16u) native->output_modes |= 1u << 3;
    if (state->c_oflag & 32u) native->output_modes |= 1u << 4;
    if (state->c_oflag & 2u) native->output_modes |= 1u << 5;
    if (state->c_oflag & 64u) native->output_modes |= 1u << 6;
    if (state->c_oflag & 128u) native->output_modes |= 1u << 7;
    if (state->c_lflag & ~122879u) return -1;
    native->local_modes = 0;
    if (state->c_lflag & 2u) native->local_modes |= 1u << 0;
    if (state->c_lflag & 1u) native->local_modes |= 1u << 1;
    if (state->c_lflag & 8u) native->local_modes |= 1u << 2;
    if (state->c_lflag & 16u) native->local_modes |= 1u << 3;
    if (state->c_lflag & 32u) native->local_modes |= 1u << 4;
    if (state->c_lflag & 64u) native->local_modes |= 1u << 5;
    if (state->c_lflag & 512u) native->local_modes |= 1u << 6;
    if (state->c_lflag & 1024u) native->local_modes |= 1u << 7;
    if (state->c_lflag & 2048u) native->local_modes |= 1u << 8;
    if (state->c_lflag & 128u) native->local_modes |= 1u << 9;
    if (state->c_lflag & 256u) native->local_modes |= 1u << 10;
    if (state->c_lflag & 32768u) native->local_modes |= 1u << 11;
    if (state->c_lflag & 4096u) native->local_modes |= 1u << 12;
    if (state->c_lflag & 16384u) native->local_modes |= 1u << 13;
    if (state->c_lflag & 65536u) native->local_modes |= 1u << 14;
    if (state->c_lflag & 4u) native->local_modes |= 1u << 15;
    uint32_t device = state->c_cflag;
    if (state->c_line || (device & ~0xc0001fffu)) return -1;
    uint32_t speed = device & 0x100fu;
    unsigned index = speed < 16 ? speed : speed >= 0x1001 && speed <= 0x100f ? speed - 0x1001 + 16 : 99;
    if (index >= sizeof(terminal_baud_rates) / sizeof(*terminal_baud_rates)) return -1;
    native->baud_rate = terminal_baud_rates[index];
    native->data_bits = 5 + ((device & 0x30u) >> 4);
    native->stop_bits = device & 0x40u ? 2 : 1;
    native->parity = !(device & 0x100u) ? 0 : device & 0x40000000u ? (device & 0x200u ? 3 : 4) : (device & 0x200u ? 1 : 2);
    native->device_modes = ((device & 0x80u) ? 1 : 0) | ((device & 0x400u) ? 2 : 0) | ((device & 0x800u) ? 4 : 0) | ((device & 0x80000000u) ? 8 : 0);
    for (unsigned i = 17; i < 19; i++) if (state->c_cc[i]) return -1;
    native->interrupt_char = state->c_cc[0];
    native->quit_char = state->c_cc[1];
    native->erase_char = state->c_cc[2];
    native->kill_char = state->c_cc[3];
    native->eof_char = state->c_cc[4];
    native->start_char = state->c_cc[8];
    native->stop_char = state->c_cc[9];
    native->suspend_char = state->c_cc[10];
    native->eol_char = state->c_cc[11];
    native->reprint_char = state->c_cc[12];
    native->discard_char = state->c_cc[13];
    native->word_erase_char = state->c_cc[14];
    native->literal_next_char = state->c_cc[15];
    native->eol2_char = state->c_cc[16];
    native->minimum_bytes = state->c_cc[6];
    native->timeout_deciseconds = state->c_cc[5];
    native->switch_char = state->c_cc[7];
    return 0;
}
static int terminal_from_native(ktty_termios_t *state, const a20_terminal_config_t *native) {
    if (native->reserved || native->reserved_byte || native->reserved_tail[0] || native->reserved_tail[1] || native->reserved_tail[2] ||
        native->data_bits < 5 || native->data_bits > 8 || native->stop_bits < 1 || native->stop_bits > 2 || native->parity > 4 || (native->device_modes & ~15u)) return -1;
    memset(state, 0, sizeof(*state));
    if (native->input_modes & ~32767u) return -1;
    if (native->input_modes & (1u << 0)) state->c_iflag |= 256u;
    if (native->input_modes & (1u << 1)) state->c_iflag |= 64u;
    if (native->input_modes & (1u << 2)) state->c_iflag |= 128u;
    if (native->input_modes & (1u << 3)) state->c_iflag |= 1u;
    if (native->input_modes & (1u << 4)) state->c_iflag |= 2u;
    if (native->input_modes & (1u << 5)) state->c_iflag |= 4u;
    if (native->input_modes & (1u << 6)) state->c_iflag |= 8u;
    if (native->input_modes & (1u << 7)) state->c_iflag |= 16u;
    if (native->input_modes & (1u << 8)) state->c_iflag |= 32u;
    if (native->input_modes & (1u << 9)) state->c_iflag |= 512u;
    if (native->input_modes & (1u << 10)) state->c_iflag |= 1024u;
    if (native->input_modes & (1u << 11)) state->c_iflag |= 2048u;
    if (native->input_modes & (1u << 12)) state->c_iflag |= 4096u;
    if (native->input_modes & (1u << 13)) state->c_iflag |= 8192u;
    if (native->input_modes & (1u << 14)) state->c_iflag |= 16384u;
    if (native->output_modes & ~255u) return -1;
    if (native->output_modes & (1u << 0)) state->c_oflag |= 1u;
    if (native->output_modes & (1u << 1)) state->c_oflag |= 4u;
    if (native->output_modes & (1u << 2)) state->c_oflag |= 8u;
    if (native->output_modes & (1u << 3)) state->c_oflag |= 16u;
    if (native->output_modes & (1u << 4)) state->c_oflag |= 32u;
    if (native->output_modes & (1u << 5)) state->c_oflag |= 2u;
    if (native->output_modes & (1u << 6)) state->c_oflag |= 64u;
    if (native->output_modes & (1u << 7)) state->c_oflag |= 128u;
    if (native->local_modes & ~65535u) return -1;
    if (native->local_modes & (1u << 0)) state->c_lflag |= 2u;
    if (native->local_modes & (1u << 1)) state->c_lflag |= 1u;
    if (native->local_modes & (1u << 2)) state->c_lflag |= 8u;
    if (native->local_modes & (1u << 3)) state->c_lflag |= 16u;
    if (native->local_modes & (1u << 4)) state->c_lflag |= 32u;
    if (native->local_modes & (1u << 5)) state->c_lflag |= 64u;
    if (native->local_modes & (1u << 6)) state->c_lflag |= 512u;
    if (native->local_modes & (1u << 7)) state->c_lflag |= 1024u;
    if (native->local_modes & (1u << 8)) state->c_lflag |= 2048u;
    if (native->local_modes & (1u << 9)) state->c_lflag |= 128u;
    if (native->local_modes & (1u << 10)) state->c_lflag |= 256u;
    if (native->local_modes & (1u << 11)) state->c_lflag |= 32768u;
    if (native->local_modes & (1u << 12)) state->c_lflag |= 4096u;
    if (native->local_modes & (1u << 13)) state->c_lflag |= 16384u;
    if (native->local_modes & (1u << 14)) state->c_lflag |= 65536u;
    if (native->local_modes & (1u << 15)) state->c_lflag |= 4u;
    unsigned index;
    for (index = 0; index < sizeof(terminal_baud_rates) / sizeof(*terminal_baud_rates); index++) if (terminal_baud_rates[index] == native->baud_rate) break;
    if (index == sizeof(terminal_baud_rates) / sizeof(*terminal_baud_rates)) return -1;
    state->c_cflag = index < 16 ? index : index - 16 + 0x1001;
    state->c_cflag |= (native->data_bits - 5) << 4;
    if (native->stop_bits == 2) state->c_cflag |= 0x40;
    if (native->parity) state->c_cflag |= 0x100;
    if (native->parity == 1 || native->parity == 3) state->c_cflag |= 0x200;
    if (native->parity >= 3) state->c_cflag |= 0x40000000u;
    if (native->device_modes & 1) state->c_cflag |= 0x80;
    if (native->device_modes & 2) state->c_cflag |= 0x400;
    if (native->device_modes & 4) state->c_cflag |= 0x800;
    if (native->device_modes & 8) state->c_cflag |= 0x80000000u;
    state->c_cc[0] = native->interrupt_char;
    state->c_cc[1] = native->quit_char;
    state->c_cc[2] = native->erase_char;
    state->c_cc[3] = native->kill_char;
    state->c_cc[4] = native->eof_char;
    state->c_cc[8] = native->start_char;
    state->c_cc[9] = native->stop_char;
    state->c_cc[10] = native->suspend_char;
    state->c_cc[11] = native->eol_char;
    state->c_cc[12] = native->reprint_char;
    state->c_cc[13] = native->discard_char;
    state->c_cc[14] = native->word_erase_char;
    state->c_cc[15] = native->literal_next_char;
    state->c_cc[16] = native->eol2_char;
    state->c_cc[6] = native->minimum_bytes;
    state->c_cc[5] = native->timeout_deciseconds;
    state->c_cc[7] = native->switch_char;
    return 0;
}

int64_t sys_a20_path_create(const a20_syscall_args_t *args)
{
    a20_path_create_args_t *uargs = (a20_path_create_args_t *)(uintptr_t)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_path_create_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    char kpath[MAX_PATH_LEN];
    if (copy_path_from_user(kpath, (const char *)(uintptr_t)kargs.path, (uint32_t)kargs.path_len) < 0)
        return -A20_ERR_FAULT;

    int dirfd;
    int64_t r = a20_dir_handle_to_dirfd(kargs.dir,
                                        A20_RIGHT_READ | A20_RIGHT_WRITE,
                                        &dirfd);
    if (r < 0) return r;

    int gfd = -1;
    if (kargs.type == 1) {
        char full[MAX_PATH_LEN];
        if (kpath[0] == '/') {
            strncpy(full, kpath, MAX_PATH_LEN);
            full[MAX_PATH_LEN - 1] = '\0';
        } else {
            char start[MAX_PATH_LEN];
            r = vfs_dirfd_path(dirfd, start, sizeof(start));
            if (r < 0) {
                if (dirfd != AT_FDCWD) fdtable_close_current(dirfd);
                return -A20_ERR_NO_ENTRY;
            }
            size_t slen = strlen(start);
            if (slen > 0 && start[slen - 1] == '/')
                snprintf(full, sizeof(full), "%s%s", start, kpath);
            else
                snprintf(full, sizeof(full), "%s/%s", start, kpath);
        }
        int mkdir_r = vfs_mkdir(full, (int)kargs.mode);
        if (mkdir_r < 0) {
            if (dirfd != AT_FDCWD) fdtable_close_current(dirfd);
            return -A20_ERR_NO_ENTRY;
        }
        gfd = vfs_openat2(dirfd, kpath, O_RDONLY | O_DIRECTORY, 0, 0);
        if (gfd < 0) {
            if (dirfd != AT_FDCWD) fdtable_close_current(dirfd);
            return -A20_ERR_NO_ENTRY;
        }
    } else {
        gfd = vfs_openat2(dirfd, kpath, O_RDWR | O_CREAT, (int)kargs.mode, 0);
        if (gfd < 0) {
            if (dirfd != AT_FDCWD) fdtable_close_current(dirfd);
            return -A20_ERR_NO_ENTRY;
        }
    }

    if (dirfd != AT_FDCWD) fdtable_close_current(dirfd);

    a20_rights_t rights = A20_RIGHT_STAT | A20_RIGHT_SEEK | A20_RIGHT_DUP |
                          A20_RIGHT_TRANSFER | A20_RIGHT_READ;
    if (kargs.type != 1)
        rights |= A20_RIGHT_WRITE;

    a20_handle_t h;
    int64_t rc = a20_install_gfd_handle(gfd,
                                        (kargs.type == 1) ? A20_OBJ_DIRECTORY
                                                          : A20_OBJ_FILE,
                                        rights, &h);
    /* The installed handle owns its own vfile reference, so the helper's
     * task fd is no longer needed here (it was leaked in the old alias
     * model until process exit). */
    fdtable_close_current(gfd);
    if (rc < 0) return rc;

    kargs.out_handle = h;
    if (a20_copy_struct_to_user(uargs, &kargs, sizeof(kargs)) < 0) {
        task_t *cur = proc_current();
        struct a20_ht_internal *ht = task_get_a20_ht(cur);
        if (ht) a20_handle_remove(ht, h);
        return -A20_ERR_FAULT;
    }
    return A20_OK;
}

int64_t sys_a20_path_unlink(const a20_syscall_args_t *args)
{
    const char *upath = (const char *)(uintptr_t)A20_ARG(0);
    uint32_t path_len = (uint32_t)A20_ARG(1);
    char kpath[MAX_PATH_LEN];
    if (copy_path_from_user(kpath, upath, path_len) < 0)
        return -A20_ERR_FAULT;

    char full[MAX_PATH_LEN];
    resolve_path(kpath, full);

    int r = vfs_unlink(full);
    if (r < 0) r = vfs_rmdir(full);
    return r;
}

int64_t sys_a20_path_rename(const a20_syscall_args_t *args)
{
    const char *uold = (const char *)(uintptr_t)A20_ARG(0);
    uint32_t old_len = (uint32_t)A20_ARG(1);
    const char *unew = (const char *)(uintptr_t)A20_ARG(2);
    uint32_t new_len = (uint32_t)A20_ARG(3);

    char kold[MAX_PATH_LEN], knew[MAX_PATH_LEN];
    if (copy_path_from_user(kold, uold, old_len) < 0) return -A20_ERR_FAULT;
    if (copy_path_from_user(knew, unew, new_len) < 0) return -A20_ERR_FAULT;

    char full_old[MAX_PATH_LEN], full_new[MAX_PATH_LEN];
    resolve_path(kold, full_old);
    resolve_path(knew, full_new);

    return vfs_rename(full_old, full_new);
}

int64_t sys_a20_path_readdir(const a20_syscall_args_t *args)
{
    a20_handle_t dir_h = (a20_handle_t)A20_ARG(0);
    void *buf = (void *)(uintptr_t)A20_ARG(1);
    size_t len = (size_t)A20_ARG(2);

    if (!buf || len == 0) return -A20_ERR_INVALID_ARGUMENT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_ref_internal(ht, dir_h, A20_OBJ_DIRECTORY,
                                               A20_RIGHT_READ, &entry);
    if (r < 0) return r;

    vfile_t *vf = (vfile_t *)entry.object;

    char kbuf[4096];
    if (len > sizeof(kbuf)) len = sizeof(kbuf);
    int64_t n = vfs_getdents64_vfile(vf, kbuf, len);
    a20_object_release(entry.object, entry.type);
    if (n < 0) return -A20_ERR_IO;


    uint64_t out_off = 0;
    uint64_t in_off = 0;
    while (in_off < (uint64_t)n) {
        vfs_dirent64_t *vd = (vfs_dirent64_t *)(kbuf + in_off);
        if (vd->d_reclen == 0) break;

        a20_dirent_t out;
        out.type = vd->d_type;
        size_t name_len = strlen(vd->d_name);
        if (name_len > sizeof(out.name) - 1)
            name_len = sizeof(out.name) - 1;
        out.name_len = (uint32_t)name_len;
        memcpy(out.name, vd->d_name, name_len);
        out.name[name_len] = '\0';

        if (out_off + sizeof(out) > len) break;
        if (copy_to_user((char *)buf + out_off, &out, sizeof(out)) < 0)
            return -A20_ERR_FAULT;
        out_off += sizeof(out);
        in_off += vd->d_reclen;
    }

    return (int64_t)out_off;
}

int64_t sys_a20_path_link(const a20_syscall_args_t *args)
{
    const char *uold = (const char *)(uintptr_t)A20_ARG(0);
    uint32_t old_len = (uint32_t)A20_ARG(1);
    const char *unew = (const char *)(uintptr_t)A20_ARG(2);
    uint32_t new_len = (uint32_t)A20_ARG(3);

    char kold[MAX_PATH_LEN], knew[MAX_PATH_LEN];
    if (copy_path_from_user(kold, uold, old_len) < 0) return -A20_ERR_FAULT;
    if (copy_path_from_user(knew, unew, new_len) < 0) return -A20_ERR_FAULT;

    char full_old[MAX_PATH_LEN], full_new[MAX_PATH_LEN];
    resolve_path(kold, full_old);
    resolve_path(knew, full_new);

    return vfs_link(full_old, full_new);
}

int64_t sys_a20_path_symlink(const a20_syscall_args_t *args)
{
    const char *utarget = (const char *)(uintptr_t)A20_ARG(0);
    uint32_t tgt_len = (uint32_t)A20_ARG(1);
    const char *ulinkpath = (const char *)(uintptr_t)A20_ARG(2);
    uint32_t lnk_len = (uint32_t)A20_ARG(3);

    char ktgt[MAX_PATH_LEN], klnk[MAX_PATH_LEN];
    if (copy_path_from_user(ktgt, utarget, tgt_len) < 0) return -A20_ERR_FAULT;
    if (copy_path_from_user(klnk, ulinkpath, lnk_len) < 0) return -A20_ERR_FAULT;

    char full_lnk[MAX_PATH_LEN];
    resolve_path(klnk, full_lnk);

    return vfs_symlink(ktgt, full_lnk);
}

int64_t sys_a20_path_readlink(const a20_syscall_args_t *args)
{
    const char *upath = (const char *)(uintptr_t)A20_ARG(0);
    uint32_t path_len = (uint32_t)A20_ARG(1);
    char *ubuf = (char *)(uintptr_t)A20_ARG(2);
    size_t bufsz = (size_t)A20_ARG(3);

    char kpath[MAX_PATH_LEN];
    if (copy_path_from_user(kpath, upath, path_len) < 0) return -A20_ERR_FAULT;

    char full[MAX_PATH_LEN];
    resolve_path(kpath, full);

    char kbuf[4096];
    int64_t r = vfs_readlinkat(-1, full, kbuf, bufsz < sizeof(kbuf) ? bufsz : sizeof(kbuf));
    if (r < 0) return -A20_ERR_NO_ENTRY;
    if (copy_to_user(ubuf, kbuf, (size_t)r) < 0) return -A20_ERR_FAULT;
    return r;
}

int64_t sys_a20_path_resolve(const a20_syscall_args_t *args)
{
    const char *upath = (const char *)(uintptr_t)A20_ARG(0);
    uint32_t path_len = (uint32_t)A20_ARG(1);

    char kpath[MAX_PATH_LEN];
    if (copy_path_from_user(kpath, upath, path_len) < 0) return -A20_ERR_FAULT;

    char full[MAX_PATH_LEN];
    resolve_path(kpath, full);

    int gfd = vfs_open(full, O_RDONLY, 0);
    if (gfd < 0) return -A20_ERR_NO_ENTRY;
    vfs_close(gfd);
    return A20_OK;
}

int64_t sys_a20_fs_stat(const a20_syscall_args_t *args)
{
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    a20_fs_stat_t *out = (a20_fs_stat_t *)(uintptr_t)A20_ARG(1);
    if (!out) return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_ref_internal(ht, h, A20_OBJ_INVALID,
                                               A20_RIGHT_STAT, &entry);
    if (r < 0) return r;

    a20_fs_stat_t fs;
    memset(&fs, 0, sizeof(fs));

    switch (entry.type) {
    case A20_OBJ_FILE:
    case A20_OBJ_DIRECTORY: {
        vfile_t *vf = (vfile_t *)entry.object;
        if (vf && vf->vnode)
            fs.block_size = 4096;
        break;
    }
    case A20_OBJ_MEMORY: {
        struct vmo *vmo = (struct vmo *)entry.object;
        fs.block_size = 4096;
        fs.total_blocks = vmo->size / 4096;
        break;
    }
    case A20_OBJ_CHANNEL_ENDPOINT: {
        a20_channel_ep_t *ep = (a20_channel_ep_t *)entry.object;
        fs.block_size = 4096;
        fs.total_files = ep->msg_count;
        break;
    }
    default:
        break;
    }
    a20_object_release(entry.object, entry.type);


    if (copy_to_user(out, &fs, sizeof(fs)) < 0) return -A20_ERR_FAULT;
    return A20_OK;
}

int64_t sys_a20_handle_control(const a20_syscall_args_t *args)
{
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    uint32_t op = (uint32_t)A20_ARG(1);
    uint64_t arg0 = A20_ARG(2);
    uint64_t arg1 = A20_ARG(3);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_ref_internal(ht, h, A20_OBJ_INVALID,
                                               A20_RIGHT_CONTROL, &entry);
    if (r < 0) return r;

    /* Temporal capability management (docs/native-abi/03-handle.md §2.6).
     * The user ABI speaks CLOCK_MONOTONIC nanoseconds; entries store ticks. */
    if (op == A20_HANDLE_CTRL_SET_TEMPORAL) {
        a20_handle_temporal_args_t *uta = (a20_handle_temporal_args_t *)(uintptr_t)arg0;
        if (!uta) {
            r = -A20_ERR_FAULT;
            goto out_entry;
        }
        a20_handle_temporal_args_t ta;
        r = a20_validate_struct_header(uta, sizeof(ta), 1);
        if (r < 0) goto out_entry;
        uint32_t usz;
        if (copy_from_user(&usz, uta, sizeof(usz)) < 0) {
            r = -A20_ERR_FAULT;
            goto out_entry;
        }
        memset(&ta, 0, sizeof(ta));
        uint32_t cpn = usz < (uint32_t)sizeof(ta) ? usz : (uint32_t)sizeof(ta);
        if (copy_from_user(&ta, uta, cpn) < 0) {
            r = -A20_ERR_FAULT;
            goto out_entry;
        }
        uint64_t expiry_tick = 0;
        if (ta.expiry_ns > 0) {
            uint64_t sec = ta.expiry_ns / 1000000000ULL;
            uint64_t nsec = ta.expiry_ns % 1000000000ULL;
            if (sec > UINT64_MAX / TICKS_PER_SEC)
                expiry_tick = UINT64_MAX / 2;
            else
                expiry_tick = sec * TICKS_PER_SEC +
                              nsec * TICKS_PER_SEC / 1000000000ULL;
        }
        a20_object_release(entry.object, entry.type);
        return a20_handle_set_temporal(ht, h, expiry_tick, ta.remaining_ops,
                                       ta.temporal_flags);
    }
    if (op == A20_HANDLE_CTRL_GET_TEMPORAL) {
        a20_handle_temporal_args_t *uta = (a20_handle_temporal_args_t *)(uintptr_t)arg0;
        if (!uta) {
            r = -A20_ERR_FAULT;
            goto out_entry;
        }
        uint64_t expiry_tick;
        uint32_t remaining_ops, temporal_flags;
        r = a20_handle_get_temporal(ht, h, &expiry_tick, &remaining_ops,
                                    &temporal_flags);
        if (r < 0) goto out_entry;
        a20_handle_temporal_args_t ta;
        memset(&ta, 0, sizeof(ta));
        ta.size = sizeof(ta);
        ta.version = 1;
        ta.expiry_ns = expiry_tick
                       ? expiry_tick * (1000000000ULL / TICKS_PER_SEC) : 0;
        ta.remaining_ops = remaining_ops;
        ta.temporal_flags = temporal_flags;
        a20_object_release(entry.object, entry.type);
        if (copy_to_user(uta, &ta, sizeof(ta)) < 0)
            return -A20_ERR_FAULT;
        return A20_OK;
    }
    if (op == A20_HANDLE_CTRL_SET_LABEL) {
        /* Versioned argument (raise-only label). */
        a20_ctl_int_args_t *ui = (a20_ctl_int_args_t *)(uintptr_t)arg0;
        if (!ui || arg1 != 0) {
            r = -A20_ERR_INVALID_ARGUMENT;
            goto out_entry;
        }
        a20_ctl_int_args_t ci;
        r = a20_validate_struct_header(ui, sizeof(ci), 1);
        if (r < 0) goto out_entry;
        if (copy_from_user(&ci, ui, sizeof(ci)) < 0) {
            r = -A20_ERR_FAULT;
            goto out_entry;
        }
        a20_object_release(entry.object, entry.type);
        return a20_handle_set_label(ht, h, (uint8_t)ci.value);
    }

    if (op == A20_HANDLE_CTRL_CHDIR) {
        if (arg0 != 0 || arg1 != 0 || entry.type != A20_OBJ_DIRECTORY) {
            r = -A20_ERR_INVALID_ARGUMENT;
            goto out_entry;
        }

        vfile_t *vf = (vfile_t *)entry.object;
        if (!vf || !vf->path[0]) {
            r = -A20_ERR_BAD_HANDLE;
            goto out_entry;
        }

        char path[MAX_PATH_LEN];
        strncpy(path, vf->path, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';

        int chdir_ret = vfs_chdir(path);
        if (chdir_ret == 0)
            r = A20_OK;
        else if (chdir_ret == -EACCES)
            r = -A20_ERR_ACCESS;
        else if (chdir_ret == -ENOENT)
            r = -A20_ERR_NO_ENTRY;
        else if (chdir_ret == -ENOTDIR)
            r = -A20_ERR_NOT_DIR;
        else if (chdir_ret == -ENAMETOOLONG)
            r = -A20_ERR_NAME_TOO_LONG;
        else
            r = -A20_ERR_IO;
        goto out_entry;
    }

    /* ---- terminal / open-file control (typed, replaces ioctl) ---- */

    if (op == A20_HANDLE_CTRL_GET_WINSIZE || op == A20_HANDLE_CTRL_SET_WINSIZE) {
        if (!a20_object_is_vfile_backed(entry.type)) { r = -A20_ERR_NOT_TERMINAL; goto out_entry; }
        a20_winsize_args_t *user = (void *)(uintptr_t)arg0;
        a20_winsize_args_t control;
        r = a20_validate_struct_header(user, sizeof(control), 1);
        if (r < 0) goto out_entry;
        if (copy_from_user(&control, user, sizeof(control)) < 0) { r = -A20_ERR_FAULT; goto out_entry; }
        ktty_winsize_t state;
        if (op == A20_HANDLE_CTRL_GET_WINSIZE) {
            r = a20_native_vfs_result(tty_vfile_get_winsize(entry.object, &state));
            if (r < 0) goto out_entry;
            control.ws_row = state.row;
            control.ws_col = state.col;
            control.ws_xpixel = state.xpixel;
            control.ws_ypixel = state.ypixel;
            r = a20_copy_struct_to_user(user, &control, sizeof(control));
        } else {
            state = (ktty_winsize_t){control.ws_row, control.ws_col, control.ws_xpixel, control.ws_ypixel};
            r = a20_native_vfs_result(tty_vfile_set_winsize(entry.object, &state));
        }
        goto out_entry;
    }

    if (op == A20_HANDLE_CTRL_GET_TERMINAL_CONFIG || op == A20_HANDLE_CTRL_SET_TERMINAL_CONFIG) {
        if (!a20_object_is_vfile_backed(entry.type)) { r = -A20_ERR_NOT_TERMINAL; goto out_entry; }
        a20_terminal_config_t *user = (void *)(uintptr_t)arg0;
        a20_terminal_config_t control;
        r = a20_validate_struct_header(user, sizeof(control), 1);
        if (r < 0) goto out_entry;
        if (copy_from_user(&control, user, sizeof(control)) < 0) { r = -A20_ERR_FAULT; goto out_entry; }
        if (control.reserved || control.reserved_byte ||
            control.reserved_tail[0] || control.reserved_tail[1] ||
            control.reserved_tail[2] || control.action > 2) {
            r = -A20_ERR_INVALID_ARGUMENT;
            goto out_entry;
        }
        ktty_termios_t state;
        if (op == A20_HANDLE_CTRL_GET_TERMINAL_CONFIG) {
            r = a20_native_vfs_result(tty_vfile_get_termios(entry.object, &state));
            if (r < 0) goto out_entry;
            if (terminal_to_native(&control, &state)) { r = -A20_ERR_NOT_SUPPORTED; goto out_entry; }
            r = a20_copy_struct_to_user(user, &control, sizeof(control));
        } else {
            if (terminal_from_native(&state, &control)) { r = -A20_ERR_INVALID_ARGUMENT; goto out_entry; }
            r = a20_native_vfs_result(tty_vfile_set_termios(entry.object, &state, control.action));
        }
        goto out_entry;
    }

    if (op == A20_HANDLE_CTRL_TCFLUSH) {
        if (!a20_object_is_vfile_backed(entry.type)) { r = -A20_ERR_NOT_TERMINAL; goto out_entry; }
        a20_ctl_int_args_t *user = (void *)(uintptr_t)arg0;
        a20_ctl_int_args_t control;
        r = a20_validate_struct_header(user, sizeof(control), 1);
        if (r < 0) goto out_entry;
        if (copy_from_user(&control, user, sizeof(control)) < 0) { r = -A20_ERR_FAULT; goto out_entry; }
        r = control.reserved ? -A20_ERR_INVALID_ARGUMENT :
            a20_native_vfs_result(tty_vfile_flush(entry.object, control.value));
        goto out_entry;
    }

    if (op == A20_HANDLE_CTRL_FILE_LOCK) {
        if (entry.type != A20_OBJ_FILE || !((vfile_t *)entry.object)->vnode ||
            ((vfile_t *)entry.object)->vnode->type != VFS_FT_REGULAR) {
            r = -A20_ERR_BAD_HANDLE;
            goto out_entry;
        }
        a20_file_lock_args_t *user = (void *)(uintptr_t)arg0, control;
        r = a20_validate_struct_header(user, sizeof(control), 1);
        if (r < 0) goto out_entry;
        if (copy_from_user(&control, user, sizeof(control)) < 0) { r = -A20_ERR_FAULT; goto out_entry; }
        if (control.reserved || control.operation > 2 || control.owner_kind > 1 ||
            (control.owner_kind && control.owner_identity)) { r = -A20_ERR_INVALID_ARGUMENT; goto out_entry; }
        if ((uint32_t)control.type > A20_RANGE_EXCLUSIVE) {
            r = -A20_ERR_INVALID_ARGUMENT; goto out_entry;
        }
        int lock_type = control.type == A20_RANGE_SHARED ? F_RDLCK : control.type == A20_RANGE_EXCLUSIVE ? F_WRLCK : F_UNLCK;
        if (control.operation && ((lock_type == F_RDLCK && !(entry.rights & A20_RIGHT_READ)) ||
                                  (lock_type == F_WRLCK && !(entry.rights & A20_RIGHT_WRITE)))) {
            r = -A20_ERR_BAD_HANDLE;
            goto out_entry;
        }
        vfile_t *file = entry.object;
        fs_flock_t lock = {lock_type, control.whence, control.start, control.length, control.owner_identity};
        int kind = control.owner_kind ? FS_LOCK_OWNER_OFD : FS_LOCK_OWNER_NATIVE;
        uintptr_t owner = control.owner_kind ? (uintptr_t)file : (uintptr_t)ht;
        r = control.operation ? fs_locks_set(file, &lock, kind, owner, control.operation == 2) :
                                fs_locks_get(file, &lock, kind, owner);
        r = a20_native_vfs_result(r);
        if (r < 0) goto out_entry;
        if (!control.operation) {
            control.type = lock.l_type == F_RDLCK ? A20_RANGE_SHARED : lock.l_type == F_WRLCK ? A20_RANGE_EXCLUSIVE : A20_RANGE_UNLOCK;
            control.whence = lock.l_whence;
            control.start = lock.l_start;
            control.length = lock.l_len;
            control.owner_identity = lock.l_pid;
            r = a20_copy_struct_to_user(user, &control, sizeof(control));
        }
        goto out_entry;
    }

    if (op == A20_HANDLE_CTRL_GET_SOCKET_CONFIG || op == A20_HANDLE_CTRL_SET_SOCKET_CONFIG) {
        if (entry.type != A20_OBJ_SOCKET) { r = -A20_ERR_NOT_SOCKET; goto out_entry; }
        a20_socket_config_t *user = (void *)(uintptr_t)arg0, control;
        r = a20_validate_struct_header(user, sizeof(control), 1);
        if (r < 0) goto out_entry;
        if (copy_from_user(&control, user, sizeof(control)) < 0) { r = -A20_ERR_FAULT; goto out_entry; }
        if (control.reserved || !control.fields || (control.fields & (control.fields - 1)) ||
            control.fields > A20_SOCKET_FAILURE) { r = -A20_ERR_INVALID_ARGUMENT; goto out_entry; }
        struct net_socket *socket = net_socket_from_vfile((vfile_t *)entry.object);
        extern int64_t a20_native_net_result(int64_t);
        int level = SOL_SOCKET, option = 0, value = 0;
        void *buffer = &value;
        size_t length = sizeof(value);
        long times[2] = {0};
        int setting = op == A20_HANDLE_CTRL_SET_SOCKET_CONFIG;
        switch (control.fields) {
        case A20_SOCKET_OPTIONS:
            if (!control.option_mask || (control.option_mask & (control.option_mask - 1)) ||
                (control.options & ~control.option_mask)) { r = -A20_ERR_INVALID_ARGUMENT; goto out_entry; }
            switch (control.option_mask) {
            case A20_SOCKET_REUSE_ADDRESS: option = SO_REUSEADDR; break;
            case A20_SOCKET_REUSE_PORT: option = SO_REUSEPORT; break;
            case A20_SOCKET_KEEP_ALIVE: option = SO_KEEPALIVE; break;
            case A20_SOCKET_NO_DELAY: level = IPPROTO_TCP; option = TCP_NODELAY; break;
            default: r = -A20_ERR_NOT_SUPPORTED; goto out_entry;
            }
            value = !!control.options;
            break;
        case A20_SOCKET_TX_CAPACITY: option = SO_SNDBUF; value = control.transmit_capacity; break;
        case A20_SOCKET_RX_CAPACITY: option = SO_RCVBUF; value = control.receive_capacity; break;
        case A20_SOCKET_RX_TIMEOUT: case A20_SOCKET_TX_TIMEOUT: {
            uint64_t ns = control.fields == A20_SOCKET_RX_TIMEOUT ? control.receive_timeout_ns : control.transmit_timeout_ns;
            if (ns / 1000000000ULL > LONG_MAX) { r = -A20_ERR_OVERFLOW; goto out_entry; }
            times[0] = ns / 1000000000ULL; times[1] = ns % 1000000000ULL / 1000;
            if (ns && !times[0] && !times[1]) times[1] = 1;
            option = control.fields == A20_SOCKET_RX_TIMEOUT ? SO_RCVTIMEO : SO_SNDTIMEO;
            buffer = times; length = sizeof(times); break;
        }
        case A20_SOCKET_HOP_LIMIT: level = IPPROTO_IP; option = IP_TTL; value = control.hop_limit; break;
        case A20_SOCKET_IDENTITY: {
            if (setting) { r = -A20_ERR_NOT_SUPPORTED; goto out_entry; }
            int family = 0, type = 0, transport = 0;
            size_t n = sizeof(int);
            int result = net_getsockopt_sock(socket, SOL_SOCKET, SO_DOMAIN, &family, &n);
            if (!result) result = net_getsockopt_sock(socket, SOL_SOCKET, SO_TYPE, &type, &n);
            if (!result) result = net_getsockopt_sock(socket, SOL_SOCKET, SO_PROTOCOL, &transport, &n);
            if (result < 0) { r = a20_native_net_result(result); goto out_entry; }
            control.family = family == AF_UNIX ? A20_SOCKET_FAMILY_LOCAL : family == AF_INET ? A20_SOCKET_FAMILY_IPV4 : family == AF_INET6 ? A20_SOCKET_FAMILY_IPV6 : 0;
            control.type = type == SOCK_STREAM ? A20_SOCKET_TYPE_STREAM : type == SOCK_DGRAM ? A20_SOCKET_TYPE_DATAGRAM : type == SOCK_SEQPACKET ? A20_SOCKET_TYPE_SEQUENCE : 0;
            if (!control.family || !control.type) { r = -A20_ERR_NOT_SUPPORTED; goto out_entry; }
            control.transport = family == AF_UNIX ? A20_SOCKET_TRANSPORT_LOCAL :
                transport == IPPROTO_TCP || (!transport && type == SOCK_STREAM) ? A20_SOCKET_TRANSPORT_TCP :
                transport == IPPROTO_UDP || (!transport && type == SOCK_DGRAM) ? A20_SOCKET_TRANSPORT_UDP : A20_SOCKET_TRANSPORT_LOCAL;
            r = a20_copy_struct_to_user(user, &control, sizeof(control)); goto out_entry;
        }
        case A20_SOCKET_FAILURE:
            if (setting) { r = -A20_ERR_NOT_SUPPORTED; goto out_entry; }
            option = SO_ERROR; break;
        default: r = -A20_ERR_NOT_SUPPORTED; goto out_entry;
        }
        r = a20_native_net_result(setting ? net_setsockopt_sock(socket, level, option, buffer, length) :
            net_getsockopt_sock(socket, level, option, buffer, &length));
        if (r < 0 || setting) goto out_entry;
        switch (control.fields) {
        case A20_SOCKET_OPTIONS: control.options = value ? control.option_mask : 0; break;
        case A20_SOCKET_TX_CAPACITY: control.transmit_capacity = value; break;
        case A20_SOCKET_RX_CAPACITY: control.receive_capacity = value; break;
        case A20_SOCKET_RX_TIMEOUT: control.receive_timeout_ns = (uint64_t)times[0] * 1000000000ULL + times[1] * 1000; break;
        case A20_SOCKET_TX_TIMEOUT: control.transmit_timeout_ns = (uint64_t)times[0] * 1000000000ULL + times[1] * 1000; break;
        case A20_SOCKET_HOP_LIMIT: control.hop_limit = value; break;
        case A20_SOCKET_FAILURE: control.failure = value ? (uint32_t)-a20_native_net_result(-value) : 0; break;
        }
        r = a20_copy_struct_to_user(user, &control, sizeof(control));
        goto out_entry;
    }

    if (op == A20_HANDLE_CTRL_SET_FLAGS || op == A20_HANDLE_CTRL_GET_FLAGS) {
        /* Change open-file flags (O_NONBLOCK etc.), like fcntl(F_SETFL) but
         * typed: valid_mask selects the bits to change. */
        if (entry.type != A20_OBJ_FILE && entry.type != A20_OBJ_DEVICE &&
            entry.type != A20_OBJ_SOCKET && entry.type != A20_OBJ_PIPE_ENDPOINT) {
            r = -A20_ERR_INVALID_ARGUMENT;
            goto out_entry;
        }
        a20_ctl_flags_args_t *uf = (a20_ctl_flags_args_t *)(uintptr_t)arg0;
        if (!uf) {
            r = -A20_ERR_FAULT;
            goto out_entry;
        }
        a20_ctl_flags_args_t cf;
        r = a20_validate_struct_header(uf, sizeof(cf), 1);
        if (r < 0) goto out_entry;
        if (copy_from_user(&cf, uf, sizeof(cf)) < 0) {
            r = -A20_ERR_FAULT;
            goto out_entry;
        }
        vfile_t *vf = (vfile_t *)entry.object;
        int current = vfs_fcntl_vfile(vf, F_GETFL, 0);
        if (current < 0) {
            r = a20_native_vfs_result(current);
            goto out_entry;
        }
        if (op == A20_HANDLE_CTRL_GET_FLAGS) {
            cf.flags = current & ~(O_CREAT | O_EXCL | O_TRUNC | O_CLOEXEC);
            cf.valid_mask = O_APPEND | O_DIRECT | O_NONBLOCK;
            r = a20_copy_struct_to_user(uf, &cf, sizeof(cf));
            goto out_entry;
        }
        uint32_t allowed = O_APPEND | O_DIRECT | O_NONBLOCK;
        if (cf.valid_mask & ~allowed) {
            r = -A20_ERR_INVALID_ARGUMENT;
            goto out_entry;
        }
        cf.flags = ((uint32_t)current & ~cf.valid_mask) | (cf.flags & cf.valid_mask);
        if (entry.type == A20_OBJ_SOCKET) {
            r = a20_native_vfs_result(
                net_set_nonblock_vfile(vf, (cf.flags & O_NONBLOCK) != 0));
            goto out_entry;
        }
        r = a20_native_vfs_result(vfs_fcntl_vfile(vf, F_SETFL, (long)cf.flags));
        goto out_entry;
    }

    if (entry.type == A20_OBJ_EVENT_QUEUE && op == 0) {
        a20_eventq_t *eq = (a20_eventq_t *)entry.object;
        if (arg0 == 1) {
            r = a20_eventq_cancel(eq, (a20_handle_t)arg1);
            goto out_entry;
        }
    }

    r = -A20_ERR_INVALID_ARGUMENT;
out_entry:
    a20_object_release(entry.object, entry.type);
    return r;
}

int64_t sys_a20_fs_mount(const a20_syscall_args_t *args)
{
    a20_fs_mount_args_t *uargs = (a20_fs_mount_args_t *)(uintptr_t)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_fs_mount_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    char ksrc[MAX_PATH_LEN], ktgt[MAX_PATH_LEN], kfs[64];
    if (copy_path_from_user(ksrc, (const char *)(uintptr_t)kargs.source, kargs.source_len) < 0)
        return -A20_ERR_FAULT;
    if (copy_path_from_user(ktgt, (const char *)(uintptr_t)kargs.target, kargs.target_len) < 0)
        return -A20_ERR_FAULT;
    if (copy_path_from_user(kfs, (const char *)(uintptr_t)kargs.fs_type, kargs.fs_type_len) < 0)
        return -A20_ERR_FAULT;

    char full_tgt[MAX_PATH_LEN];
    resolve_path(ktgt, full_tgt);

    int r = vfs_mount(ksrc[0] ? ksrc : NULL, full_tgt,
                       kfs[0] ? kfs : "ext2", (int)kargs.flags, NULL);
    if (r < 0) return -A20_ERR_IO;
    return A20_OK;
}

int64_t sys_a20_fs_umount(const a20_syscall_args_t *args)
{
    const char *utarget = (const char *)(uintptr_t)A20_ARG(0);
    uint32_t tgt_len = (uint32_t)A20_ARG(1);
    uint64_t flags = A20_ARG(2);
    (void)flags;

    char ktgt[MAX_PATH_LEN];
    if (copy_path_from_user(ktgt, utarget, tgt_len) < 0) return -A20_ERR_FAULT;

    char full_tgt[MAX_PATH_LEN];
    resolve_path(ktgt, full_tgt);

    int r = vfs_umount(full_tgt);
    if (r < 0) return -A20_ERR_IO;
    return A20_OK;
}

int64_t sys_a20_fs_sync(const a20_syscall_args_t *args)
{
    (void)args;
    return vfs_sync();
}

int64_t sys_a20_path_unlink_at(const a20_syscall_args_t *args)
{
    a20_path_unlink_args_t *uargs = (a20_path_unlink_args_t *)(uintptr_t)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;
    a20_path_unlink_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);
    if (kargs.flags & ~AT_REMOVEDIR)
        return -A20_ERR_INVALID_ARGUMENT;

    char full[MAX_PATH_LEN];
    int64_t r = a20_native_copy_join(kargs.dir, kargs.path,
                                     kargs.path_len, full);
    if (r < 0) return r;
    int vr = (kargs.flags & AT_REMOVEDIR) ? vfs_rmdir(full) : vfs_unlink(full);
    return a20_native_vfs_result(vr);
}

int64_t sys_a20_path_rename_at(const a20_syscall_args_t *args)
{
    a20_path_rename_args_t *uargs = (a20_path_rename_args_t *)(uintptr_t)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;
    a20_path_rename_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    char old_path[MAX_PATH_LEN], new_path[MAX_PATH_LEN];
    int64_t r = a20_native_copy_join(kargs.old_dir, kargs.old_path,
                                     kargs.old_path_len, old_path);
    if (r < 0) return r;
    r = a20_native_copy_join(kargs.new_dir, kargs.new_path,
                             kargs.new_path_len, new_path);
    if (r < 0) return r;
    return a20_native_vfs_result(vfs_rename_flags(old_path, new_path,
                                                   kargs.flags));
}

int64_t sys_a20_path_link_at(const a20_syscall_args_t *args)
{
    a20_path_link_args_t *uargs = (a20_path_link_args_t *)(uintptr_t)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;
    a20_path_link_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);
    /* AT_SYMLINK_FOLLOW is the default for linkat (vfs_link already resolves
     * the source through symlinks); reject any other flag. */
    if (kargs.flags & ~AT_SYMLINK_FOLLOW)
        return -A20_ERR_INVALID_ARGUMENT;

    char old_path[MAX_PATH_LEN], new_path[MAX_PATH_LEN];
    int64_t r = a20_native_copy_join(kargs.old_dir, kargs.old_path,
                                     kargs.old_path_len, old_path);
    if (r < 0) return r;
    r = a20_native_copy_join(kargs.new_dir, kargs.new_path,
                             kargs.new_path_len, new_path);
    if (r < 0) return r;
    return a20_native_vfs_result(vfs_link(old_path, new_path));
}

int64_t sys_a20_path_symlink_at(const a20_syscall_args_t *args)
{
    a20_path_symlink_args_t *uargs = (a20_path_symlink_args_t *)(uintptr_t)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;
    a20_path_symlink_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    char target[MAX_PATH_LEN], link_path[MAX_PATH_LEN];
    if (copy_path_from_user(target, (const char *)(uintptr_t)kargs.target,
                             kargs.target_len) < 0)
        return -A20_ERR_FAULT;
    int64_t r = a20_native_copy_join(kargs.dir, kargs.linkpath,
                                     kargs.linkpath_len, link_path);
    if (r < 0) return r;
    return a20_native_vfs_result(vfs_symlink(target, link_path));
}

int64_t sys_a20_path_readlink_at(const a20_syscall_args_t *args)
{
    a20_path_readlink_args_t *uargs = (a20_path_readlink_args_t *)(uintptr_t)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;
    a20_path_readlink_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    char full[MAX_PATH_LEN];
    int64_t r = a20_native_copy_join(kargs.dir, kargs.path,
                                     kargs.path_len, full);
    if (r < 0) return r;
    if (kargs.buf_len > 0 && !kargs.buf)
        return -A20_ERR_FAULT;

    char buffer[4096];
    size_t size = kargs.buf_len < sizeof(buffer) ? (size_t)kargs.buf_len : sizeof(buffer);
    int vr = vfs_readlinkat(-1, full, buffer, size);
    if (vr < 0)
        return a20_native_vfs_result(vr);
    if (vr > 0 && copy_to_user((void *)(uintptr_t)kargs.buf, buffer, (size_t)vr) < 0)
        return -A20_ERR_FAULT;
    kargs.out_len = (uint64_t)vr;
    if (a20_copy_struct_to_user(uargs, &kargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}

/* ------------------------------------------------------------------ */
/* uxfs: user-space file service registration and controlled block IO     */
/* ------------------------------------------------------------------ */

int64_t sys_a20_fs_serve(const a20_syscall_args_t *args)
{
    a20_fs_serve_args_t *uargs = (a20_fs_serve_args_t *)(uintptr_t)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;
    a20_fs_serve_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    char ktgt[MAX_PATH_LEN];
    if (copy_path_from_user(ktgt, (const char *)(uintptr_t)kargs.target,
                            kargs.target_len) < 0)
        return -A20_ERR_FAULT;
    char full_tgt[MAX_PATH_LEN];
    resolve_path(ktgt, full_tgt);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_ref_internal(
        ht, kargs.server_channel, A20_OBJ_CHANNEL_ENDPOINT,
        A20_RIGHT_READ | A20_RIGHT_WRITE, &entry);
    if (r < 0) return r;

    /*
     * Success path: the endpoint object reference taken by lookup_ref is
     * handed to the uxfs mount to own, and returned on umount through
     * uxfs_unmount -> a20_channel_ep_release.
     */
    r = uxfs_serve_mount(full_tgt, (a20_channel_ep_t *)entry.object, cur,
                         (int)kargs.block_index, kargs.flags);
    if (r < 0)
        a20_object_release(entry.object, A20_OBJ_CHANNEL_ENDPOINT);
    return (r < 0) ? -A20_ERR_IO : A20_OK;
}

#define UFS_BLOCK_IO_MAX_BYTES (1024u * 1024u)

int64_t sys_a20_fs_block_io(const a20_syscall_args_t *args)
{
    a20_fs_block_io_args_t *uargs = (a20_fs_block_io_args_t *)(uintptr_t)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;
    a20_fs_block_io_args_t kargs;
    int validate = a20_validate_struct_header(uargs, sizeof(kargs), 2);
    if (validate < 0) return validate;
    if (copy_from_user(&kargs, uargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;

    task_t *cur = proc_current();

    /* count == 0/write == 2 is a flush request. It needs no user buffer;
     * devices without an explicit durability operation report unsupported. */
    if (kargs.count == 0) {
        if (kargs.write == 2 && kargs.buf == 0)
            return a20_native_vfs_result(
                uxfs_block_flush(cur, (int)kargs.block_index));
        /* UXFS extension capability query. The output is 0/1 and lets
         * optional-flush clients leave their callback NULL when absent. */
        if (kargs.version == 2 && kargs.write == 2 && kargs.buf != 0) {
            int supported = uxfs_block_flush_supported(
                cur, (int)kargs.block_index);
            if (supported < 0)
                return a20_native_vfs_result(supported);
            /* Fixed-width magic response prevents a legacy u64 capacity
             * value from being mistaken for this capability. */
            uint64_t out = 0x5546585300000000ull | (uint32_t)supported;
            if (copy_to_user((void *)(uintptr_t)kargs.buf, &out,
                             sizeof(out)) < 0)
                return -A20_ERR_FAULT;
            return A20_OK;
        }
        /* Preserve the v1 capacity query for every non-flush write value:
         * older callers treated this field as a boolean and ignored it here. */
        if (!kargs.buf)
            return -A20_ERR_INVALID_ARGUMENT;
        uint64_t sectors = 0;
        int qrc = uxfs_block_capacity(cur, (int)kargs.block_index,
                                      &sectors);
        if (qrc < 0)
            return -A20_ERR_IO;
        uint64_t out = sectors;
        if (copy_to_user((void *)(uintptr_t)kargs.buf, &out, sizeof(out)) < 0)
            return -A20_ERR_FAULT;
        return A20_OK;
    }

    if (!kargs.buf || kargs.count > 4096)
        return -A20_ERR_INVALID_ARGUMENT;

    uint32_t secsz = 512; /* probe with 512 first; the real sector size is
                          * verified once the device is in hand */
    size_t bytes = (size_t)kargs.count * secsz;
    if (bytes > UFS_BLOCK_IO_MAX_BYTES)
        return -A20_ERR_INVALID_ARGUMENT;

    void *kbuf = kmalloc(bytes);
    if (!kbuf) return -A20_ERR_NO_MEMORY;

    if (kargs.write &&
        copy_from_user(kbuf, (const void *)(uintptr_t)kargs.buf, bytes) < 0) {
        kfree(kbuf);
        return -A20_ERR_FAULT;
    }

    int rc = uxfs_block_io(cur, (int)kargs.block_index, (int)kargs.write,
                           kargs.lba, kbuf, kargs.count);
    if (rc < 0) {
        printf("[UXFS] block_io idx=%d write=%d lba=%llu count=%u rc=%d\n",
               (int)kargs.block_index, (int)kargs.write,
               (unsigned long long)kargs.lba, kargs.count, rc);
        kfree(kbuf);
        return -A20_ERR_IO;
    }

    if (!kargs.write &&
        copy_to_user((void *)(uintptr_t)kargs.buf, kbuf, bytes) < 0) {
        kfree(kbuf);
        return -A20_ERR_FAULT;
    }
    kfree(kbuf);
    return A20_OK;
}
