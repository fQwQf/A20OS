/*
 * A20OS Native ABI — hypervisor VM / vcpu syscalls (0x0F00).
 *
 * Design record: docs/hypervisor/00-design.md S5.  The kernel-side objects
 * and their lifetime rules are frozen in kernel/include/hyp/hyp_vcpu.h; this
 * file only turns them into capability calls: a VM and a vcpu are objects, so
 * they live in the caller's handle table and travel as handles, never as
 * pointers (docs/native-abi/03-handle.md §2).
 *
 * Both VM and vCPU are ordinary typed capability objects. Lookup pins an
 * object under the handle-table lock; each syscall drops that temporary pin.
 * Closing, transferring, duplicating and task exit use the shared lifetime.
 */
#include "core/types.h"
#include "core/defs.h"
#include "core/string.h"
#include "core/consts.h"
#include "core/errno.h"
#include "proc/proc.h"
#include "mm/slab.h"
#include "sys/usercopy.h"

#include "abi/native/types.h"
#include "abi/native/errno.h"
#include "abi/native/rights.h"
#include "abi/native/syscall_entry.h"
#include "abi/native/handle_table.h"
#include "ipc/handle_table.h"
#include "hyp/hyp.h"
#include "hyp/hyp_vcpu.h"

#define A20_ARG(n) (args->arg[(n)])

extern struct a20_ht_internal *task_get_a20_ht(task_t *t);
extern int64_t a20_handle_install(struct a20_ht_internal *ht, void *object,
                                  uint16_t type, a20_rights_t rights);
extern int64_t a20_handle_lookup_internal(struct a20_ht_internal *ht,
                                          a20_handle_t h,
                                          uint16_t expected_type,
                                          a20_rights_t required_rights,
                                          a20_handle_entry_t *out);
extern int64_t a20_handle_remove(struct a20_ht_internal *ht, a20_handle_t h);

/* Stable object type aliases shared with the handle-table dispatcher. */
#define A20_HT_TYPE_HYP_VM    A20_OBJ_HYP_VM
#define A20_HT_TYPE_HYP_VCPU  A20_OBJ_HYP_VCPU

static struct a20_ht_internal *hyp_ht(void)
{
    struct a20_ht_internal *ht = task_get_a20_ht(proc_current());
    return ht;
}

/* Resolve one handle into its object.  The type slot separates VM from vcpu
 * (and from nothing else this slice installs), and the magic check is what
 * actually makes the cast safe: a handle of any other class that a caller
 * names here fails it instead of being reinterpreted as a hyp object. */
static int64_t hyp_obj(struct a20_ht_internal *ht, a20_handle_t h,
                       uint16_t type, uint32_t magic, a20_rights_t rights, void **out)
{
    a20_handle_entry_t e;
    int64_t r = a20_handle_lookup_ref_internal(ht, h, type, rights, &e);
    if (r < 0)
        return r;
    if (!e.object || *(const uint32_t *)e.object != magic) {
        a20_object_release(e.object, e.type);
        return -A20_ERR_BAD_HANDLE;
    }
    *out = e.object;
    return A20_OK;
}

static hyp_vm_t *hyp_get_vm(struct a20_ht_internal *ht, a20_handle_t h, a20_rights_t rights)
{
    void *obj = NULL;
    if (hyp_obj(ht, h, A20_HT_TYPE_HYP_VM, HYP_VM_MAGIC, rights, &obj) < 0)
        return NULL;
    return (hyp_vm_t *)obj;
}

static hyp_vcpu_t *hyp_get_vcpu(struct a20_ht_internal *ht, a20_handle_t h)
{
    void *obj = NULL;
    if (hyp_obj(ht, h, A20_HT_TYPE_HYP_VCPU, HYP_VCPU_MAGIC, A20_RIGHT_CONTROL, &obj) < 0)
        return NULL;
    return (hyp_vcpu_t *)obj;
}

/* A20_SYS_hyp_vm_create(mem_size) -> vm handle */
int64_t sys_a20_hyp_vm_create(const a20_syscall_args_t *args)
{
    if (!hyp_supported())
        return -A20_ERR_NOT_SUPPORTED;
    uint64_t mem_size = A20_ARG(0);
    if (mem_size == 0)
        return -A20_ERR_INVALID_ARGUMENT;

    struct a20_ht_internal *ht = hyp_ht();
    if (!ht)
        return -A20_ERR_BAD_HANDLE;

    hyp_vm_t *vm = hyp_vm_create(mem_size);
    if (!vm)
        return -A20_ERR_NO_MEMORY;

    int64_t h = a20_handle_install(ht, vm, A20_HT_TYPE_HYP_VM,
                                   (A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_CONTROL |
                                    A20_RIGHT_STAT | A20_RIGHT_DUP | A20_RIGHT_TRANSFER));
    if (h < 0) {
        hyp_vm_put(vm);
        return h;
    }
    return h;
}

/*
 * A20_SYS_hyp_vm_load(vm, gpa, buf, len) -> 0
 *
 * hyp_vm_load() takes a kernel pointer, so the payload is staged one page at
 * a time.  gpa must be page-aligned (the frozen contract requires it, and the
 * stage-2 walk only takes whole pages); the final chunk is short, and
 * hyp_vm_load() rounds the length up into that page.
 */
#define HYP_LOAD_MAX (16 * 1024 * 1024)

/* A20_ERR <-> Linux errno mapping, same shape as sys_native_mm.c's. */
static int64_t hyp_errno_map(int r)
{
    switch (r) {
    case -ENOMEM: return -A20_ERR_NO_MEMORY;
    case -EFAULT: return -A20_ERR_FAULT;
    case -EIO: return -A20_ERR_IO;
    case -EOPNOTSUPP: return -A20_ERR_NOT_SUPPORTED;
    case -EBUSY:  return -A20_ERR_BUSY;
    default:      return -A20_ERR_INVALID_ARGUMENT;
    }
}

/* Overwrite a staging page through a volatile pointer.  No explicit_bzero() in
 * this tree, and a memset() on a buffer that is dead right after is a store
 * the compiler may delete; the slab page itself outlives this call. */
static void hyp_stage_scrub(void *page)
{
    volatile uint8_t *q = (volatile uint8_t *)page;
    for (uint64_t i = 0; i < PAGE_SIZE; i++)
        q[i] = 0;
}

int64_t sys_a20_hyp_vm_load(const a20_syscall_args_t *args)
{
    struct a20_ht_internal *ht = hyp_ht();
    if (!ht)
        return -A20_ERR_BAD_HANDLE;

    uint64_t gpa = A20_ARG(1);
    const void *ubuf = (const void *)(uintptr_t)A20_ARG(2);
    uint64_t len = A20_ARG(3);
    if (!ubuf || len == 0)
        return -A20_ERR_FAULT;
    if (gpa & (PAGE_SIZE - 1))
        return -A20_ERR_INVALID_ARGUMENT;
    if (len > HYP_LOAD_MAX)
        return -A20_ERR_RANGE;

    hyp_vm_t *vm = hyp_get_vm(ht, (a20_handle_t)A20_ARG(0), A20_RIGHT_WRITE);
    if (!vm)
        return -A20_ERR_BAD_HANDLE;

    /* Staged off the stack on purpose: 4 KiB of automatic array is 6% of a
     * 64 KiB task stack, on a path that already sits several frames deep. */
    uint8_t *page = kmalloc(PAGE_SIZE);
    if (!page) {
        hyp_vm_put(vm);
        return -A20_ERR_NO_MEMORY;
    }

    int64_t rc = A20_OK;
    uint64_t done = 0;
    while (done < len) {
        uint64_t chunk = len - done;
        if (chunk > PAGE_SIZE)
            chunk = PAGE_SIZE;
        if (copy_from_user(page, (const char *)ubuf + done, chunk) < 0) {
            rc = -A20_ERR_FAULT;
            break;
        }
        int r = hyp_vm_load(vm, gpa + done, page, chunk);
        if (r < 0) {
            rc = hyp_errno_map(r);
            break;
        }
        done += chunk;
    }

    hyp_stage_scrub(page);
    kfree(page);
    hyp_vm_put(vm);
    return rc;
}

/* A20_SYS_hyp_vcpu_create(vm, entry_gpa) -> vcpu handle */
int64_t sys_a20_hyp_vcpu_create(const a20_syscall_args_t *args)
{
    if (!hyp_supported())
        return -A20_ERR_NOT_SUPPORTED;

    struct a20_ht_internal *ht = hyp_ht();
    if (!ht)
        return -A20_ERR_BAD_HANDLE;

    hyp_vm_t *vm = hyp_get_vm(ht, (a20_handle_t)A20_ARG(0), A20_RIGHT_CONTROL);
    if (!vm)
        return -A20_ERR_BAD_HANDLE;

    hyp_vcpu_t *vcpu = hyp_vcpu_create(vm, A20_ARG(1));
    hyp_vm_put(vm);
    if (!vcpu)
        return -A20_ERR_NO_MEMORY;

    int64_t h = a20_handle_install(ht, vcpu, A20_HT_TYPE_HYP_VCPU,
                                   (A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_CONTROL |
                                    A20_RIGHT_STAT | A20_RIGHT_DUP | A20_RIGHT_TRANSFER));
    if (h < 0) {
        hyp_vcpu_put(vcpu);
        return h;
    }
    return h;
}

/*
 * A20_SYS_hyp_vcpu_run(vcpu) -> exit reason (>= 0) or a negative status.
 *
 * hyp_vcpu_run() itself answers 0 for "the guest exited, see vcpu->exit", so
 * the ABI hands the reason back instead: a user program that has to tell
 * shutdown from a guest fault would otherwise have no way to.
 */
int64_t sys_a20_hyp_vcpu_run(const a20_syscall_args_t *args)
{
    if (!hyp_supported())
        return -A20_ERR_NOT_SUPPORTED;

    struct a20_ht_internal *ht = hyp_ht();
    if (!ht)
        return -A20_ERR_BAD_HANDLE;

    hyp_vcpu_t *vcpu = hyp_get_vcpu(ht, (a20_handle_t)A20_ARG(0));
    if (!vcpu)
        return -A20_ERR_BAD_HANDLE;

    /* Past the probe above, a negative return is an internal failure of the
     * run loop itself -- never the "no H extension" answer. */
    hyp_exit_reason_t reason = HYP_EXIT_NONE;
    int rc = hyp_vcpu_run_result(vcpu, &reason);
    int64_t result = rc < 0 ? hyp_errno_map(rc) : (int64_t)reason;
    hyp_vcpu_put(vcpu);
    return result;
}

/* A20_SYS_hyp_vm_destroy(vm) -> 0.  Vcpus of this VM keep it alive through
 * the reference hyp_vcpu_create() took, so only the caller's own reference
 * goes here. */
int64_t sys_a20_hyp_vm_destroy(const a20_syscall_args_t *args)
{
    struct a20_ht_internal *ht = hyp_ht();
    if (!ht)
        return -A20_ERR_BAD_HANDLE;

    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    hyp_vm_t *vm = hyp_get_vm(ht, h, A20_RIGHT_CONTROL);
    if (!vm)
        return -A20_ERR_BAD_HANDLE;

    int64_t r = a20_handle_remove(ht, h);
    hyp_vm_put(vm); /* Drop lookup pin; remove already dropped handle ownership. */
    return r;
}

int64_t sys_a20_hyp_vcpu_destroy(const a20_syscall_args_t *args)
{
    struct a20_ht_internal *ht = hyp_ht();
    if (!ht)
        return -A20_ERR_BAD_HANDLE;
    hyp_vcpu_t *vcpu = hyp_get_vcpu(ht, (a20_handle_t)A20_ARG(0));
    if (!vcpu)
        return -A20_ERR_BAD_HANDLE;
    int64_t rc = a20_handle_remove(ht, (a20_handle_t)A20_ARG(0));
    hyp_vcpu_put(vcpu);
    return rc;
}

int64_t sys_a20_hyp_console(const a20_syscall_args_t *args)
{
    uint64_t op = args->arg[1], len = args->arg[3];
    void *user = (void *)(uintptr_t)args->arg[2];
    uint8_t data[256];
    int rc;
    struct a20_ht_internal *ht = hyp_ht();
    if (!ht)
        return -A20_ERR_BAD_HANDLE;
    a20_rights_t rights = op == HYP_CONSOLE_CONFIG ? A20_RIGHT_CONTROL :
                          op == HYP_CONSOLE_READ ? A20_RIGHT_READ : A20_RIGHT_WRITE;
    hyp_vm_t *vm = hyp_get_vm(ht, (a20_handle_t)args->arg[0], rights);
    if (!vm)
        return -A20_ERR_BAD_HANDLE;
    if (op == HYP_CONSOLE_CONFIG) {
        rc = args->arg[2] > 1 || len ? -EINVAL :
             hyp_console_config(vm, (int)args->arg[2]);
    } else if (len > sizeof(data)) {
        rc = -EINVAL;
    } else if (op == HYP_CONSOLE_WRITE) {
        rc = copy_from_user(data, user, len) < 0 ? -EFAULT :
             hyp_console_write(vm, data, (unsigned)len);
    } else if (op == HYP_CONSOLE_READ) {
        rc = hyp_console_read(vm, data, (unsigned)len);
        if (rc > 0 && copy_to_user(user, data, rc) < 0)
            rc = -EFAULT;
    } else {
        rc = -EINVAL;
    }
    hyp_vm_put(vm);
    return rc < 0 ? hyp_errno_map(rc) : rc;
}

int64_t sys_a20_hyp_vm_set_ram(const a20_syscall_args_t *args)
{
    struct a20_ht_internal *ht = hyp_ht();
    if (!ht)
        return -A20_ERR_BAD_HANDLE;
    hyp_vm_t *vm = hyp_get_vm(ht, (a20_handle_t)args->arg[0], A20_RIGHT_CONTROL);
    if (!vm)
        return -A20_ERR_BAD_HANDLE;
    int rc = hyp_vm_set_ram(vm, args->arg[1], args->arg[2]);
    hyp_vm_put(vm);
    return rc < 0 ? hyp_errno_map(rc) : rc;
}

int64_t sys_a20_hyp_vcpu_set_boot(const a20_syscall_args_t *args)
{
    struct a20_ht_internal *ht = hyp_ht();
    if (!ht)
        return -A20_ERR_BAD_HANDLE;
    hyp_vcpu_t *vcpu = hyp_get_vcpu(ht, (a20_handle_t)args->arg[0]);
    if (!vcpu)
        return -A20_ERR_BAD_HANDLE;
    int rc = hyp_vcpu_set_boot(vcpu, args->arg[1], args->arg[2]);
    hyp_vcpu_put(vcpu);
    return rc < 0 ? hyp_errno_map(rc) : rc;
}

int64_t sys_a20_hyp_vm_set_marker(const a20_syscall_args_t *args)
{
    struct a20_ht_internal *ht = hyp_ht();
    if (!ht)
        return -A20_ERR_BAD_HANDLE;
    char marker[HYP_VM_MARKER_MAX + 1];
    if (copy_from_user(marker, (void *)(uintptr_t)args->arg[1], HYP_VM_MARKER_MAX) < 0)
        return -A20_ERR_FAULT;
    marker[HYP_VM_MARKER_MAX] = 0;
    if (strlen(marker) == HYP_VM_MARKER_MAX)
        return -A20_ERR_INVALID_ARGUMENT;
    hyp_vm_t *vm = hyp_get_vm(ht, (a20_handle_t)args->arg[0], A20_RIGHT_CONTROL);
    if (!vm)
        return -A20_ERR_BAD_HANDLE;
    int rc = hyp_vm_set_marker(vm, marker);
    hyp_vm_put(vm);
    return rc < 0 ? hyp_errno_map(rc) : rc;
}

int64_t sys_a20_hyp_vm_status(const a20_syscall_args_t *args)
{
    struct a20_ht_internal *ht = hyp_ht();
    if (!ht)
        return -A20_ERR_BAD_HANDLE;
    hyp_vm_t *vm = hyp_get_vm(ht, (a20_handle_t)args->arg[0], A20_RIGHT_READ);
    if (!vm)
        return -A20_ERR_BAD_HANDLE;
    struct hyp_vm_status status;
    hyp_vm_get_status(vm, &status);
    hyp_vm_put(vm);
    return copy_to_user((void *)(uintptr_t)args->arg[1], &status, sizeof(status)) < 0 ?
           -A20_ERR_FAULT : A20_OK;
}

int64_t sys_a20_hyp_vcpu_control(const a20_syscall_args_t *args)
{
    struct a20_ht_internal *ht = hyp_ht();
    if (!ht) return -A20_ERR_BAD_HANDLE;
    hyp_vcpu_t *vcpu = hyp_get_vcpu(ht, (a20_handle_t)args->arg[0]);
    if (!vcpu) return -A20_ERR_BAD_HANDLE;
    int rc = args->arg[1] > HYP_CONTROL_STOP ? -EINVAL :
             hyp_vcpu_control(vcpu, (unsigned)args->arg[1]);
    hyp_vcpu_put(vcpu);
    return rc < 0 ? hyp_errno_map(rc) : rc;
}

int64_t sys_a20_hyp_vcpu_irq(const a20_syscall_args_t *args)
{
    struct a20_ht_internal *ht = hyp_ht();
    if (!ht) return -A20_ERR_BAD_HANDLE;
    hyp_vcpu_t *vcpu = hyp_get_vcpu(ht, (a20_handle_t)args->arg[0]);
    if (!vcpu) return -A20_ERR_BAD_HANDLE;
    int rc = args->arg[1] > HYP_IRQ_EXTERNAL || args->arg[2] > 1 ? -EINVAL :
             hyp_vcpu_inject_irq(vcpu, (unsigned)args->arg[1], (int)args->arg[2]);
    hyp_vcpu_put(vcpu);
    return rc < 0 ? hyp_errno_map(rc) : rc;
}

int64_t sys_a20_hyp_stats(const a20_syscall_args_t *args)
{
    struct hyp_stats stats;
    hyp_stats_get(&stats);
    return copy_to_user((void *)(uintptr_t)args->arg[0], &stats, sizeof(stats)) < 0 ?
           -A20_ERR_FAULT : A20_OK;
}

int64_t sys_a20_hyp_vm_smp(const a20_syscall_args_t *args)
{
    struct a20_ht_internal *ht = hyp_ht();
    if (!ht) return -A20_ERR_BAD_HANDLE;
    hyp_vm_t *vm = hyp_get_vm(ht, (a20_handle_t)args->arg[0], A20_RIGHT_CONTROL);
    if (!vm) return -A20_ERR_BAD_HANDLE;
    int rc = args->arg[1] > HYP_MAX_VCPUS ? -EINVAL :
             hyp_vm_configure_smp(vm, (unsigned)args->arg[1]);
    hyp_vm_put(vm);
    return rc < 0 ? hyp_errno_map(rc) : rc;
}
