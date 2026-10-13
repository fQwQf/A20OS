/* Host firmware performs synchronous remote HFENCE in M mode. It must
 * service its own IPIs independently of HS locks/interrupt enable state. */
#include "hyp/hyp_vcpu.h"
#include "core/smp.h"
#include "core/errno.h"
#include "core/string.h"
struct hyp_sbiret { int64_t error; uint64_t value; };
static struct hyp_sbiret call(uint64_t eid, uint64_t fid, uint64_t x0,
                              uint64_t x1, uint64_t x2, uint64_t x3)
{
    register uint64_t a0 __asm__("a0") = x0;
    register uint64_t a1 __asm__("a1") = x1;
    register uint64_t a2 __asm__("a2") = x2;
    register uint64_t a3 __asm__("a3") = x3;
    register uint64_t a6 __asm__("a6") = fid;
    register uint64_t a7 __asm__("a7") = eid;
    __asm__ volatile("ecall" : "+r"(a0), "+r"(a1)
                     : "r"(a2), "r"(a3), "r"(a6), "r"(a7) : "memory");
    return (struct hyp_sbiret){ (int64_t)a0, a1 };
}
int hyp_riscv_probe_smp(hyp_vm_t *vm)
{
    (void)vm;
    struct hyp_sbiret ret = call(0x10, 3, 0x52464e43, 0, 0, 0);
    if (ret.error || !ret.value) return -EOPNOTSUPP;
    const smp_cpu_desc_t *cpu = smp_cpu_desc(cpu_current_id());
    if (!cpu) return -EOPNOTSUPP;
    /* Probe the specific HFENCE function, not just the RFENCE extension.
     * A self-targeted full flush cannot expose or free any guest frame. */
    ret = call(0x52464e43, 4, 1, cpu->hw_id, 0, (uint64_t)-1);
    return ret.error ? -EOPNOTSUPP : 0;
}
int hyp_riscv_remote_fence(hyp_vm_t *vm, uint32_t cpus, unsigned kind)
{
    (void)vm;
    if (kind > 2) return -EINVAL;
    uint64_t fid = kind == 0 ? 0 : kind == 1 ? 6 : 4;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    for (unsigned id = 0; id < CONFIG_NR_CPUS; id++) {
        if (!(cpus & (1u << id))) continue;
        const smp_cpu_desc_t *cpu = smp_cpu_desc(id);
        if (!cpu || !smp_cpu_is_online(id)) return -EIO;
        struct hyp_sbiret ret = call(0x52464e43, fid, 1, cpu->hw_id, 0, (uint64_t)-1);
        if (ret.error) return -EIO;
    }
    return 0;
}
void hyp_riscv_reset(hyp_vcpu_t *vcpu, uint64_t entry, uint64_t hart, uint64_t opaque)
{
    memset(vcpu->regs, 0, sizeof(vcpu->regs));
    vcpu->arch[23] = 0; /* restart in VS supervisor mode */
    memset(vcpu->fpu, 0, sizeof(vcpu->fpu));
    vcpu->pc = entry;
    vcpu->regs[10] = hart; vcpu->regs[11] = opaque;
    vcpu->vsstatus = vcpu->vsie = vcpu->vstvec = vcpu->vsscratch = 0;
    vcpu->vsepc = vcpu->vscause = vcpu->vstval = vcpu->vsatp = 0;
    vcpu->fcsr = vcpu->hvip = 0;
    vcpu->timer_deadline = (uint64_t)-1;
    vcpu->exit = HYP_EXIT_NONE;
}
