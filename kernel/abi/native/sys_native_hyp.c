/*
 * A20OS Native ABI — hypervisor VM / vcpu syscalls (0x0F00).
 *
 * Design record: docs/hypervisor/00-design.md S5.  The kernel-side objects
 * and their lifetime rules are frozen in kernel/include/hyp/hyp_vcpu.h; this
 * file only turns them into capability calls: a VM and a vcpu are objects, so
 * they live in the caller's handle table and travel as handles, never as
 * pointers (docs/native-abi/03-handle.md §2).
 *
 * Handle typing.  The object-type enum (kernel/include/ipc/ipc.h) has no VM
 * or vcpu class yet, so these entries carry a private type value.  It has no
 * rights mask (a20_type_valid_rights() answers 0 above A20_OBJ_VMAR), which is
 * why every entry here is looked up with required_rights 0 and identified by
 * its magic instead.  Consequence, stated rather than hidden: the table's
 * a20_object_ref/release arms are no-ops for these types, so the references
 * hyp_vm_create()/hyp_vcpu_create() handed us are owned by THIS layer, and
 * hyp_vm_destroy() drops the VM's.  Two gaps follow from that and are named
 * here rather than papered over: this slice's syscall set has no vcpu-destroy,
 * so a vcpu -- and the VM reference it holds -- is never dropped; and a
 * caller that exits without destroying its VM leaks it.  Both close when the
 * object enum and its ref/release dispatch land.
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

/* Private handle types; see the typing note in the file header. */
#define A20_HT_TYPE_HYP_VM    18
#define A20_HT_TYPE_HYP_VCPU  19

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
                       uint16_t type, uint32_t magic, void **out)
{
    a20_handle_entry_t e;
    int64_t r = a20_handle_lookup_internal(ht, h, type, 0, &e);
    if (r < 0)
        return r;
    if (!e.object || *(const uint32_t *)e.object != magic)
        return -A20_ERR_BAD_HANDLE;
    *out = e.object;
    return A20_OK;
}

static hyp_vm_t *hyp_get_vm(struct a20_ht_internal *ht, a20_handle_t h)
{
    void *obj = NULL;
    if (hyp_obj(ht, h, A20_HT_TYPE_HYP_VM, HYP_VM_MAGIC, &obj) < 0)
        return NULL;
    return (hyp_vm_t *)obj;
}

static hyp_vcpu_t *hyp_get_vcpu(struct a20_ht_internal *ht, a20_handle_t h)
{
    void *obj = NULL;
    if (hyp_obj(ht, h, A20_HT_TYPE_HYP_VCPU, HYP_VCPU_MAGIC, &obj) < 0)
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

    int64_t h = a20_handle_install(ht, vm, A20_HT_TYPE_HYP_VM, 0);
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

    hyp_vm_t *vm = hyp_get_vm(ht, (a20_handle_t)A20_ARG(0));
    if (!vm)
        return -A20_ERR_BAD_HANDLE;

    /* Staged off the stack on purpose: 4 KiB of automatic array is 6% of a
     * 64 KiB task stack, on a path that already sits several frames deep. */
    uint8_t *page = kmalloc(PAGE_SIZE);
    if (!page)
        return -A20_ERR_NO_MEMORY;

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

    hyp_vm_t *vm = hyp_get_vm(ht, (a20_handle_t)A20_ARG(0));
    if (!vm)
        return -A20_ERR_BAD_HANDLE;

    hyp_vcpu_t *vcpu = hyp_vcpu_create(vm, A20_ARG(1));
    if (!vcpu)
        return -A20_ERR_NO_MEMORY;

    int64_t h = a20_handle_install(ht, vcpu, A20_HT_TYPE_HYP_VCPU, 0);
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
    if (hyp_vcpu_run(vcpu) < 0)
        return -A20_ERR_IO;
    return (int64_t)vcpu->exit;
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
    hyp_vm_t *vm = hyp_get_vm(ht, h);
    if (!vm)
        return -A20_ERR_BAD_HANDLE;

    int64_t r = a20_handle_remove(ht, h);
    if (r < 0)
        return r;
    hyp_vm_put(vm);
    return A20_OK;
}