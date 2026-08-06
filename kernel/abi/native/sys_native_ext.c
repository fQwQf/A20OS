/*
 * A20OS Native ABI — Kernel extension points (0x0D00).
 *
 * Thin wrapper over the KEP framework (kernel/ext/kep.c): load a verified
 * extension program, attach it to a kernel extension point, detach or
 * release it.  The program id is wrapped in an A20_OBJ_EXT_PROG handle so
 * the capability model (dup/transfer/close) applies; closing the last
 * handle releases the program.
 *
 * Rights (docs/native-abi/06-security.md): READ allows attach/detach,
 * CONTROL allows load/release.  An extension program only ever sees the
 * typed context of the extension point it is attached to, and is verified
 * before acceptance (forward jumps only, context-bound memory access), so
 * no extension can corrupt kernel state.
 */

#include "core/types.h"
#include "core/defs.h"
#include "core/string.h"
#include "ext/kep.h"
#include "abi/native/types.h"
#include "abi/native/objects.h"
#include "abi/native/errno.h"
#include "abi/native/rights.h"
#include "abi/native/syscall_entry.h"
#include "abi/native/ipc_internal.h"
#include "abi/native/handle_table.h"
#include "sys/usercopy.h"

#define A20_ARG(n) (args->arg[(n)])

/* Resolve the extension-program handle to its kernel program id. */
static int64_t a20_ext_prog_id(struct a20_ht_internal *ht, a20_handle_t h,
                               a20_rights_t rights)
{
    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, h, A20_OBJ_EXT_PROG, rights,
                                           &entry);
    if (r < 0)
        return r;
    return (int64_t)(intptr_t)entry.object;
}

int64_t sys_a20_ext_prog_load(const a20_syscall_args_t *args)
{
    const bpf_insn_t *instr = (const bpf_insn_t *)A20_ARG(0);
    uint32_t len = (uint32_t)A20_ARG(1);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    int id = kep_prog_load(instr, len);
    if (id < 0)
        return -A20_ERR_INVALID_ARGUMENT;

    int64_t h = a20_handle_install(ht, (void *)(intptr_t)id, A20_OBJ_EXT_PROG,
                                   A20_RIGHT_READ | A20_RIGHT_DUP |
                                   A20_RIGHT_TRANSFER | A20_RIGHT_CONTROL);
    if (h < 0) {
        (void)kep_prog_release(id);
        return -A20_ERR_NO_MEMORY;
    }
    return h;
}

int64_t sys_a20_ext_prog_attach(const a20_syscall_args_t *args)
{
    a20_handle_t prog_h = (a20_handle_t)A20_ARG(0);
    uint32_t point_id = (uint32_t)A20_ARG(1);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    int64_t id = a20_ext_prog_id(ht, prog_h, A20_RIGHT_READ);
    if (id < 0)
        return id;

    int r = kep_prog_attach((int)id, point_id);
    if (r == -ENOENT) return -A20_ERR_BAD_HANDLE;
    if (r == -EINVAL) return -A20_ERR_INVALID_ARGUMENT;
    if (r < 0) return -A20_ERR_NO_MEMORY;
    return A20_OK;
}

int64_t sys_a20_ext_prog_detach(const a20_syscall_args_t *args)
{
    a20_handle_t prog_h = (a20_handle_t)A20_ARG(0);
    uint32_t point_id = (uint32_t)A20_ARG(1);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    int64_t id = a20_ext_prog_id(ht, prog_h, A20_RIGHT_READ);
    if (id < 0)
        return id;

    int r = kep_prog_detach((int)id, point_id);
    if (r == -ENOENT) return -A20_ERR_BAD_HANDLE;
    return A20_OK;
}

int64_t sys_a20_ext_prog_release(const a20_syscall_args_t *args)
{
    a20_handle_t prog_h = (a20_handle_t)A20_ARG(0);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    int64_t id = a20_ext_prog_id(ht, prog_h, A20_RIGHT_CONTROL);
    if (id < 0)
        return id;

    /* Detach everywhere and drop the owner reference; then the handle is
     * gone as well (program dies with its last reference). */
    (void)kep_prog_detach((int)id, 0);
    (void)kep_prog_release((int)id);
    (void)a20_handle_remove(ht, prog_h);
    return A20_OK;
}

int64_t sys_a20_ext_point_info(const a20_syscall_args_t *args)
{
    uint32_t point_id = (uint32_t)A20_ARG(0);
    a20_ext_point_info_t *out = (a20_ext_point_info_t *)A20_ARG(1);
    if (!out) return -A20_ERR_FAULT;

    a20_ext_point_info_t info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    info.version = 1;

    /* The registry lives behind kep.c; expose id/name/nwords through a
     * tiny query helper. */
    int r = kep_point_query(point_id, info.name, sizeof(info.name),
                            &info.nwords);
    if (r < 0)
        return -A20_ERR_BAD_HANDLE;
    info.id = point_id;
    if (copy_to_user(out, &info, sizeof(info)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}
