/* Shared RV32/RV64 virtual SBI. No guest pointer is a host pointer. */
#include "hyp/hyp_sbi.h"
#include "mm/frame.h"

#define SBI_SUCCESS 0
#define SBI_FAILED (-1)
#define SBI_NOT_SUPPORTED (-2)
#define SBI_INVALID_PARAM (-3)
#define SBI_DENIED (-4)
#define SBI_INVALID_ADDRESS (-5)
#define SBI_ALREADY_AVAILABLE (-6)

static uint64_t arg(hyp_vcpu_t *v, unsigned n)
{
    return (reg_t)v->regs[10 + n];
}
static int sbi_error(int rc)
{
    switch (rc) {
    case 0: return SBI_SUCCESS;
    case -EOPNOTSUPP: return SBI_NOT_SUPPORTED;
    case -EINVAL: case -ERANGE: return SBI_INVALID_PARAM;
    case -EFAULT: case -EACCES: return SBI_INVALID_ADDRESS;
    case -EPERM: return SBI_DENIED;
    case -EALREADY: return SBI_ALREADY_AVAILABLE;
    default: return SBI_FAILED;
    }
}
static uint64_t timer_arg(hyp_vcpu_t *v)
{
#if defined(CONFIG_32BIT)
    return arg(v, 0) | (arg(v, 1) << 32);
#else
    return arg(v, 0);
#endif
}
static void set_timer(hyp_vcpu_t *v)
{
    uint64_t deadline = timer_arg(v);
    v->timer_deadline = deadline;
    /* TIME must also clear any software-injected timer pending state. Sstc
     * drives the hardware VSTIP from the comparator itself. */
    hyp_vcpu_inject_irq(v, HYP_IRQ_TIMER, 0);
    hyp_vcpu_sync_irqs(v);
    hyp_arch_vstimecmp_set(deadline);
}
static int extension(uint32_t eid)
{
    switch (eid) {
    case 0: case 1: case 2: case 8:
    case HYP_SBI_BASE: case HYP_SBI_TIME: case HYP_SBI_HSM:
    case HYP_SBI_IPI: case HYP_SBI_RFENCE: case HYP_SBI_SRST:
    case HYP_SBI_DBCN: return 1;
    default: return 0;
    }
}
static int shutdown(hyp_vcpu_t *v, uint64_t cause)
{
    v->exit_scause = cause;
    hyp_vm_shutdown(v->vm);
    v->exit = HYP_EXIT_SHUTDOWN;
    return 0;
}
static int put_byte(hyp_vcpu_t *v)
{
    int rc = hyp_console_putchar(v->vm, (uint8_t)arg(v, 0));
    if (rc == -EAGAIN) {
        /* Blocking byte calls retry without advancing PC or changing args. */
        v->exit = HYP_EXIT_YIELD;
    }
    return rc;
}

/* Validate the entire supplied buffer before console side effects. Unmapped
 * RAM has the demand-fill RWX policy; existing leaves must allow the access.
 * Configuration cannot change during RUN and the access itself pins backing.
 * Only normal RAM is allowed: never MMIO, page tables or arbitrary host PA. */
static int console_range(hyp_vm_t *vm, uint64_t gpa, uint64_t len, pte_t need)
{
    if (len > HYP_SBI_CONSOLE_MAX_BUFFER || len > vm->ram_size ||
        gpa < vm->ram_base || gpa - vm->ram_base > vm->ram_size - len)
        return SBI_INVALID_PARAM;
    if (!len) return 0;
    uint64_t last = gpa + len - 1;
    for (uint64_t page = gpa & ~(uint64_t)(PAGE_SIZE - 1);;
         page += PAGE_SIZE) {
        pte_t prot = hyp_riscv_s2_prot(vm, page);
        if (prot && (prot & need) != need) return SBI_INVALID_PARAM;
        if (last - page < PAGE_SIZE) break;
    }
    return 0;
}
static int debug_console(hyp_vcpu_t *v, int read, uint64_t *done)
{
    uint64_t len = arg(v, 0), gpa = arg(v, 1), high = arg(v, 2);
#if defined(CONFIG_32BIT)
    gpa |= high << 32;
#else
    if (high) return SBI_INVALID_PARAM;
#endif
    int err = console_range(v->vm, gpa, len, read ? PTE_W : PTE_R);
    if (err) return err;
    if (len > HYP_SBI_CONSOLE_QUANTUM) len = HYP_SBI_CONSOLE_QUANTUM;
    while (*done < len) {
        uint64_t page = gpa & ~(uint64_t)(PAGE_SIZE - 1);
        if (hyp_ram_fill(v->vm, page)) return SBI_FAILED;
        pfn_t pfn;
        int rc = hyp_riscv_s2_pin_access(v->vm, gpa, read ? PTE_W : PTE_R, &pfn);
        if (rc) return rc == -EACCES ? SBI_INVALID_PARAM : SBI_FAILED;
        uint8_t *p = (uint8_t *)pfn_to_virt(pfn) + (gpa & (PAGE_SIZE - 1));
        uint64_t count = PAGE_SIZE - (gpa & (PAGE_SIZE - 1));
        if (count > len - *done) count = len - *done;
        uint64_t n = 0;
        while (n < count) {
            int c = read ? hyp_console_getchar(v->vm) :
                           hyp_console_putchar(v->vm, p[n]);
            if (c < 0) {
                frame_put(pfn);
                *done += n;
                return c == -EAGAIN ? SBI_SUCCESS : SBI_FAILED;
            }
            if (read) p[n] = (uint8_t)c;
            n++;
        }
        frame_put(pfn);
        gpa += n;
        *done += n;
    }
    return SBI_SUCCESS;
}

int hyp_riscv_sbi_ecall(hyp_vcpu_t *v, uint64_t scause)
{
    /* EID/FID are 32-bit identifiers even on RV64. */
    uint32_t eid = (uint32_t)v->regs[17], fid = (uint32_t)v->regs[16];
    uint64_t a0 = arg(v, 0), a1 = arg(v, 1), result = 0;
    int error = SBI_NOT_SUPPORTED;
    if (eid < HYP_SBI_BASE) {
        switch (eid) {
        case 0: set_timer(v); error = 0; break;
        case 1: {
            int rc = put_byte(v);
            if (rc == -EAGAIN) return 0;
            error = rc ? SBI_FAILED : 0;
            break;
        }
        case 2: {
            int c = hyp_console_getchar(v->vm);
            /* Legacy getchar returns -1 when no input is available. */
            error = c < 0 ? -1 : c;
            break;
        }
        case 8: return shutdown(v, scause);
        default: break; /* Unsupported calls must not kill the guest. */
        }
        v->regs[10] = (reg_t)error;
        /* Legacy ABI preserves a1, including RV32 timer's high word. */
        v->pc = (reg_t)(v->pc + 4);
        return 1;
    }
    switch (eid) {
    case HYP_SBI_BASE:
        error = 0;
        switch (fid) {
        case 0: result = HYP_SBI_SPEC_VERSION; break;
        case 1: result = HYP_SBI_IMPL_ID; break;
        case 2: result = HYP_SBI_IMPL_VERSION; break;
        case 3: result = extension((uint32_t)a0); break;
        case 4: case 5: case 6: break; /* Virtual machine identity: zero is legal. */
        default: error = SBI_NOT_SUPPORTED;
        }
        break;
    case HYP_SBI_TIME:
        if (!fid) { set_timer(v); error = 0; }
        break;
    case HYP_SBI_HSM:
        if (!fid) {
            if (hyp_vm_hart_status(v->vm, a0) < 0) error = SBI_INVALID_PARAM;
            else {
                pte_t prot = hyp_riscv_s2_prot(v->vm, a1);
                if ((a1 & 1) || !hyp_vm_ram_contains(v->vm, a1) ||
                    (prot && !(prot & PTE_X))) error = SBI_INVALID_ADDRESS;
                else error = sbi_error(hyp_vm_hart_start(v->vm, a0, a1, arg(v, 2)));
            }
        } else if (fid == 1) {
            __atomic_store_n(&v->hart_state, HYP_HART_STOP_PENDING, __ATOMIC_RELEASE);
            v->exit = HYP_EXIT_HART_STOPPED;
            return 0;
        } else if (fid == 2) {
            int rc = hyp_vm_hart_status(v->vm, a0);
            error = rc < 0 ? sbi_error(rc) : 0;
            if (!error) result = (unsigned)rc;
        }
        break;
    case HYP_SBI_IPI:
    case HYP_SBI_RFENCE: {
        uint64_t base = a1 == (reg_t)-1 ? (uint64_t)-1 : a1;
        if (eid == HYP_SBI_IPI && !fid)
            error = sbi_error(hyp_vm_ipi(v->vm, a0, base));
        else if (eid == HYP_SBI_RFENCE && fid <= 2) {
            /* Flush all addresses/ASIDs, which safely covers the requested
             * range. Inactive harts flush both translations and I$ on load. */
            error = sbi_error(hyp_vm_fence(v->vm, a0, base, fid ? 1 : 0));
            if (!error) hyp_riscv_local_fence(fid ? 1 : 0);
        }
        break;
    }
    case HYP_SBI_SRST:
        if (!fid) {
            uint32_t type = (uint32_t)a0, reason = (uint32_t)a1;
            if (type > 2 || reason > 1) error = SBI_INVALID_PARAM;
            else if (!type) return shutdown(v, scause);
            /* Cold/warm reset require a VMM reset protocol; never reset host. */
            else error = SBI_NOT_SUPPORTED;
        }
        break;
    case HYP_SBI_DBCN:
        if (fid <= 1) error = debug_console(v, fid == 1, &result);
        else if (fid == 2) {
            int rc = put_byte(v);
            if (rc == -EAGAIN) return 0;
            error = rc ? SBI_FAILED : 0;
        }
        break;
    }
    v->regs[10] = (reg_t)error;
    v->regs[11] = (reg_t)result;
    v->pc = (reg_t)(v->pc + 4);
    return 1;
}
