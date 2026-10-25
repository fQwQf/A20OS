/* Intel SDM vol. 3C, chapters 24-31: VMCS, VM entry/exit and EPT.
 * The boot ABI is shared with SVM, using VMCALL in place of VMMCALL.
 * VMCS launch state is cleared on put, allowing safe physical CPU migration. */
#include "hyp/hyp_vcpu.h"
#include "hyp/hyp_s2.h"
#include "core/errno.h"
#include "core/string.h"
#include "core/cpu.h"
#include "core/timer.h"
#include "mm/slab.h"
#include "cpu.h"
struct vmx_state {
    pfn_t pages; /* VMXON, VMCS, host FX/guest FX, reserved */
    unsigned initialized, launched, active;
    uint64_t old_cr0, old_cr4, old_dr6, old_dr7, old_cr2;
};
static uint64_t readvm(unsigned field)
{ uint64_t v; __asm__ volatile("vmread %1,%0" : "=r"(v) : "r"((uint64_t)field) : "cc"); return v; }
static int writevm(unsigned field, uint64_t v)
{ unsigned char bad; __asm__ volatile("vmwrite %2,%1; setna %0" : "=qm"(bad) : "r"((uint64_t)field), "r"(v) : "cc"); return bad; }
static uint32_t controls(unsigned msr, uint32_t want)
{ uint64_t caps = rdmsr(msr); return (want | (uint32_t)caps) & (caps >> 32); }
static unsigned ctl_msr(unsigned legacy)
{ return (rdmsr(0x480) & BIT(55)) ? legacy + 0xc : legacy; }
int hyp_vmx_supported(void)
{
#if !defined(ARCH_HAS_PGTABLE_OPS) || defined(CONFIG_NOMMU)
    return 0;
#endif
    uint32_t a,b,c,d;
    __asm__ volatile("cpuid" : "=a"(a),"=b"(b),"=c"(c),"=d"(d) : "a"(1),"c"(0));
    if (!(c & BIT(5)) || (d & (BIT(24)|BIT(25))) != (BIT(24)|BIT(25))) return 0;
    uint64_t fc = rdmsr(0x3a), basic = rdmsr(0x480);
    if ((fc & 1) && !(fc & 4)) return 0;
    if (((basic >> 50) & 15) != 6 || ((basic >> 32) & 0x1fff) > PAGE_SIZE) return 0;
    /* VMX timer bounds noncooperative SMP guests even if the host's
     * one-shot physical IRQ is masked or unavailable during VM execution. */
    if (!(controls(ctl_msr(0x481), BIT(6)) & BIT(6))) return 0;
    if ((rdmsr(0x485) & 31) > 10) return 0;
    uint32_t primary = controls(ctl_msr(0x482), BIT(31));
    if (!(primary & BIT(31))) return 0;
    uint32_t secondary = controls(0x48b, BIT(1)|BIT(7));
    if ((secondary & (BIT(1)|BIT(7))) != (BIT(1)|BIT(7))) return 0;
    /* Four-level WB EPT and all-context INVEPT are mandatory. */
    uint64_t ept = rdmsr(0x48c);
    if ((ept & (BIT(6)|BIT(14)|BIT(20)|BIT(26))) != (BIT(6)|BIT(14)|BIT(20)|BIT(26))) return 0;
    return 1;
}
static int vmx_invalidate(hyp_vm_t *vm, uint64_t gpa)
{ (void)vm; (void)gpa; return 0; }
static uint64_t vmx_leaf(pte_t prot)
{
    if (!(prot & PTE_R)) return 0; /* common ownership requires readable RAM */
    return BIT(0) | ((prot & PTE_W) ? BIT(1):0) | ((prot & PTE_X) ? BIT(2):0) | (6u << 3);
}
static const hyp_s2_format_t ept_format = {
    .levels=4, .address_mask=0x000ffffffffff000ULL, .valid_mask=1,
    .table_flags=7, .leaf_check_mask=0xfff, .live_update=1,
    .leaf_flags=vmx_leaf, .invalidate=vmx_invalidate,
};
static int vmx_vm_init(hyp_vm_t *vm) { return hyp_s2_tree_init(vm, &ept_format); }
static void vmx_reset(hyp_vcpu_t *v, uint64_t entry, uint64_t id, uint64_t opaque)
{
    struct vmx_state *s=v->backend_data;
    if (s) {
        s->initialized=0;
        uint8_t *pages=pfn_to_virt(s->pages);
        memset(pages+PAGE_SIZE,0,2*PAGE_SIZE);
        *(uint32_t *)(pages+PAGE_SIZE)=rdmsr(0x480)&0x7fffffff;
        uint8_t *fx=pages+2*PAGE_SIZE+512;
        *(uint16_t *)fx=0x37f;*(uint32_t *)(fx+24)=0x1f80;
        *(uint64_t *)(fx+520)=0xffff0ff0ULL;
    }
    memset(v->regs,0,sizeof(v->regs)); v->pc=entry; v->regs[6]=opaque; v->regs[7]=id;
}
static int vmx_boot(hyp_vcpu_t *v,uint64_t id,uint64_t opaque)
{ v->regs[6]=opaque; v->regs[7]=id; return 0; }
static void vmx_destroy(hyp_vcpu_t *v)
{ struct vmx_state *s=v->backend_data; if(s) {pfa_free(s->pages,2);kfree(s);v->backend_data=NULL;} }
static uint16_t selector(unsigned which)
{
    uint16_t v=0;
    switch(which) {
    case 0: __asm__ volatile("mov %%es,%0":"=r"(v)); break;
    case 1: __asm__ volatile("mov %%cs,%0":"=r"(v)); break;
    case 2: __asm__ volatile("mov %%ss,%0":"=r"(v)); break;
    case 3: __asm__ volatile("mov %%ds,%0":"=r"(v)); break;
    case 4: __asm__ volatile("mov %%fs,%0":"=r"(v)); break;
    case 5: __asm__ volatile("mov %%gs,%0":"=r"(v)); break;
    case 6: __asm__ volatile("str %0":"=r"(v)); break;
    }
    return v & ~7u;
}
static void vmx_host(void)
{
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) gdtr,idtr;
    __asm__ volatile("sgdt %0; sidt %1":"=m"(gdtr),"=m"(idtr));
    const unsigned fields[]={0xc00,0xc02,0xc04,0xc06,0xc08,0xc0a,0xc0c};
    for(unsigned i=0;i<7;i++) writevm(fields[i],selector(i));
    uint64_t cr0,cr3,cr4;
    __asm__ volatile("mov %%cr0,%0; mov %%cr3,%1; mov %%cr4,%2":"=r"(cr0),"=r"(cr3),"=r"(cr4));
    writevm(0x6c00,cr0&~12ULL);writevm(0x6c02,cr3);writevm(0x6c04,cr4);
    writevm(0x6c06,rdmsr(0xc0000100));writevm(0x6c08,rdmsr(0xc0000101));
    uint8_t *tr=(uint8_t *)(uintptr_t)(gdtr.base+selector(6));
    uint64_t trbase=*(uint16_t *)(tr+2) | ((uint64_t)tr[4]<<16) |
        ((uint64_t)tr[7]<<24) | ((uint64_t)*(uint32_t *)(tr+8)<<32);
    writevm(0x6c0a,trbase);writevm(0x6c0c,gdtr.base);writevm(0x6c0e,idtr.base);
    writevm(0x4c00,rdmsr(0x174));writevm(0x6c10,rdmsr(0x175));writevm(0x6c12,rdmsr(0x176));
    writevm(0x2c00,rdmsr(0x277));writevm(0x2c02,rdmsr(0xc0000080));
}
static int vmx_init_guest(hyp_vcpu_t *v)
{
    /* 32-bit protected, unpaged guest requires unrestricted-guest EPT. */
    for(unsigned i=0;i<8;i++) {
        writevm(0x800+i*2,i==1?8:(i<6?16:0));
        writevm(0x4800+i*2,i<6?0xffffffffu:0xffff);
        writevm(0x4814+i*2,i==1?0xc09b:(i<6?0xc093:(i==6?0x10000:0x8b)));
        writevm(0x6806+i*2,0);
    }
    writevm(0x4810,0xffff);writevm(0x4812,0xffff);
    writevm(0x6816,0);writevm(0x6818,0);
    writevm(0x6800,0x31);writevm(0x6802,0);writevm(0x6804,(0x600|rdmsr(0x488))&rdmsr(0x489));
    writevm(0x681a,0x400);writevm(0x6820,2);
    writevm(0x4824,0);writevm(0x4826,0);writevm(0x4828,0);writevm(0x482a,0);
    writevm(0x2800,~0ULL);writevm(0x2802,0);writevm(0x2804,0x0007040600070406ULL);
    writevm(0x2806,0); /* EFER */
    writevm(0x482a,0);writevm(0x6824,0);writevm(0x6826,0); /* SYSENTER */
    writevm(0x4004,0);writevm(0x4006,0);writevm(0x4008,0);writevm(0x400a,0);
    writevm(0x6000,~0ULL);writevm(0x6002,~0ULL); /* CR0/4 software masks */
    writevm(0x6004,0x31);writevm(0x6006,0x600);
    writevm(0x400e,0);writevm(0x4010,0);writevm(0x4014,0);writevm(0x4016,0);writevm(0x4018,0);
    (void)v; return 0;
}
static void vmx_put(hyp_vcpu_t *v)
{
    struct vmx_state *s=v->backend_data;
    if(s->active) {
        uint64_t pa=pfn_to_phys(s->pages)+PAGE_SIZE;
        __asm__ volatile("vmclear %0; vmxoff"::"m"(pa):"cc","memory");s->active=0;
    }
    __asm__ volatile("mov %0,%%dr6; mov %1,%%dr7; mov %2,%%cr2; mov %3,%%cr4; mov %4,%%cr0"
        ::"r"(s->old_dr6),"r"(s->old_dr7),"r"(s->old_cr2),"r"(s->old_cr4),"r"(s->old_cr0):"memory");
    /* A VMX preemption-timer exit may precede a physical one-shot whose
     * count exhausted while guest IRQ delivery was suppressed. Ensure the
     * host scheduler still has a deadline after ownership returns to it. */
    x86_64_timer_ensure_deadline(TICKS_PER_SEC/100);
}
static int vmx_load(hyp_vcpu_t *v)
{
    if(!hyp_vmx_supported())return -EOPNOTSUPP;
    if(!v||!v->vm||!v->vm->s2_root)return -EINVAL;
    struct vmx_state *s=v->backend_data;
    if(!s) {
        s=kcalloc(1,sizeof(*s));if(!s)return -ENOMEM;
        s->pages=pfa_alloc(2);if(s->pages==PFN_NONE){kfree(s);return -ENOMEM;}
        memset(pfn_to_virt(s->pages),0,4*PAGE_SIZE);
        *(uint32_t *)pfn_to_virt(s->pages)=rdmsr(0x480)&0x7fffffff;
        *(uint32_t *)((uint8_t *)pfn_to_virt(s->pages)+PAGE_SIZE)=rdmsr(0x480)&0x7fffffff;
        uint8_t *fx=(uint8_t *)pfn_to_virt(s->pages)+2*PAGE_SIZE+512;
        *(uint64_t *)(fx+520)=0xffff0ff0ULL;
        *(uint16_t *)fx=0x37f;*(uint32_t *)(fx+24)=0x1f80;
        v->backend_data=s;
    }
    if(!s->initialized&&v->pc>0xffffffffu)return -EINVAL;
    uint64_t fc=rdmsr(0x3a);if(!(fc&1))wrmsr(0x3a,fc|5);
    __asm__ volatile("mov %%cr0,%0;mov %%cr4,%1;mov %%dr6,%2;mov %%dr7,%3;mov %%cr2,%4"
        :"=r"(s->old_cr0),"=r"(s->old_cr4),"=r"(s->old_dr6),"=r"(s->old_dr7),"=r"(s->old_cr2));
    uint64_t cr0=(s->old_cr0|rdmsr(0x486))&rdmsr(0x487);
    uint64_t cr4=(s->old_cr4|rdmsr(0x488)|BIT(13)|BIT(9)|BIT(10))&rdmsr(0x489);
    __asm__ volatile("mov %0,%%cr0;mov %1,%%cr4"::"r"(cr0),"r"(cr4):"memory");
    uint64_t on=pfn_to_phys(s->pages),vmcs=on+PAGE_SIZE;unsigned char bad;
    __asm__ volatile("vmxon %1;setna %0":"=qm"(bad):"m"(on):"cc","memory");
    if(bad){vmx_put(v);return -EIO;}s->active=1;
    __asm__ volatile("vmclear %1;setna %0":"=qm"(bad):"m"(vmcs):"cc","memory");
    if(!bad)__asm__ volatile("vmptrld %1;setna %0":"=qm"(bad):"m"(vmcs):"cc","memory");
    if(bad){vmx_put(v);return -EIO;}
    if(!s->initialized){vmx_init_guest(v);s->initialized=1;}
    writevm(0x4000,controls(ctl_msr(0x481),BIT(0)|BIT(6)));
    writevm(0x4002,controls(ctl_msr(0x482),BIT(7)|BIT(23)|BIT(24)|BIT(31)));
    writevm(0x401e,controls(0x48b,BIT(1)|BIT(7)));
    writevm(0x400c,controls(ctl_msr(0x483),BIT(2)|BIT(9)|BIT(18)|BIT(19)|BIT(20)|BIT(21)));
    writevm(0x4012,controls(ctl_msr(0x484),BIT(2)|BIT(14)|BIT(15)));
    writevm(0x201a,((uint64_t)(uintptr_t)v->vm->s2_root-PAGE_OFFSET)|6|(3u<<3));
    vmx_host();s->launched=0;
    x86_64_timer_ensure_deadline(TICKS_PER_SEC/100);
    return 0;
}
extern int hyp_vmx_run(uint64_t *regs,void *host_fx,void *guest_fx,unsigned resume);
static void vmx_cpuid(hyp_vcpu_t *v)
{
    uint32_t leaf=v->regs[0],a=0,b=0,c=0,d=0;
    if(leaf==0){a=1;b=0x756e6547;d=0x49656e69;c=0x6c65746e;}
    if(leaf==1){a=0x663;b=1u<<16;c=1u<<31;d=0x0780a179;}
    if(leaf==0x80000000)a=0x80000008;
    if(leaf==0x80000008)a=48|(48u<<8);
    v->regs[0]=a;v->regs[3]=b;v->regs[1]=c;v->regs[2]=d;
}
static int vmx_msr(hyp_vcpu_t *v,int write)
{
    unsigned field;
    switch((uint32_t)v->regs[1]) {
    case 0xc0000080:field=0x2806;break;
    case 0x277:field=0x2804;break;
    case 0x174:field=0x482a;break;
    case 0x175:field=0x6824;break;
    case 0x176:field=0x6826;break;
    default:return 0;
    }
    uint64_t value=(uint32_t)v->regs[0]|((uint64_t)(uint32_t)v->regs[2]<<32);
    if(write){
        if(field==0x2806 && value&~1ULL)return 0; /* 32-bit VMX guest ABI */
        if(field==0x2804)return 0; /* retain valid default memory types */
        writevm(field,value);
    }else{value=readvm(field);v->regs[0]=(uint32_t)value;v->regs[2]=value>>32;}
    return 1;
}
static void vmx_enter(hyp_vcpu_t *v)
{
    struct vmx_state *s=v->backend_data;
    uint8_t *fx=(uint8_t *)pfn_to_virt(s->pages)+2*PAGE_SIZE;
    for(unsigned budget=0;budget<256;budget++) {
        if(hyp_vcpu_control_exit(v))return;
        /* Global invalidation handles ASID reuse, migration and local fill. */
        struct {uint64_t eptp,reserved;} desc={0,0};uint64_t kind=2;
        __asm__ volatile("invept %0,%1"::"m"(desc),"r"(kind):"cc","memory");
        writevm(0x482e,10000);
        writevm(0x681e,v->pc);writevm(0x681c,v->regs[4]);
        if(hyp_vmx_run(v->regs,fx,fx+512,s->launched)){
            v->exit_scause=readvm(0x4400);v->exit=HYP_EXIT_ERROR;return;
        }
        s->launched=1;v->pc=readvm(0x681e);v->regs[4]=readvm(0x681c);
        uint64_t reason=readvm(0x4402),qual=readvm(0x6400);
        v->exit_scause=reason;v->exit_stval=qual;v->exit_htval=readvm(0x2400);
        if(reason&BIT(31)){v->exit=HYP_EXIT_ERROR;return;}
        reason&=0xffff;
        unsigned advance=0;
        if(reason==1 || reason==52){v->exit=HYP_EXIT_YIELD;return;}
        if(reason==10){vmx_cpuid(v);advance=1;}
        else if(reason==31||reason==32)advance=vmx_msr(v,reason==32);
        else if(reason==18){
            if (v->regs[0] == 9) {
                v->regs[0] = (uint64_t)(int64_t)hyp_vm_hart_start(v->vm,
                    v->regs[3], v->regs[1], v->regs[2]); advance = 1;
            } else if (v->regs[0] == 10) {
                v->exit = HYP_EXIT_HART_STOPPED; return;
            } else if (v->regs[0] == 11) {
                v->regs[0] = (uint64_t)(int64_t)hyp_vm_hart_status(v->vm, v->regs[3]); advance = 1;
            } else
            if(v->regs[0]==8){v->exit=HYP_EXIT_SHUTDOWN;return;}
            if(v->regs[0]==1){
                if(hyp_console_putchar(v->vm,v->regs[3])){v->exit=HYP_EXIT_YIELD;return;}
                v->regs[0]=0;advance=1;
            }
        }else if(reason==12){v->pc+=readvm(0x440c);v->exit=HYP_EXIT_YIELD;return;}
        else if(reason==28 && ((qual>>4)&3)==0) {
            unsigned cr=qual&15,reg=(qual>>8)&15;uint64_t val=v->regs[reg];
            if(cr==0 && !(val&~0xe005003fULL) && (val&1)){
                writevm(0x6800,val|0x20);writevm(0x6004,val);advance=1;
            } else if(cr==4 && !(val&~0x6ffULL)){
                writevm(0x6804,(val|rdmsr(0x488))&rdmsr(0x489));writevm(0x6006,val);advance=1;
            }
        }else if(reason==48 && !(qual&0x38)){
            if(!hyp_ram_fill(v->vm,readvm(0x2400)&~(uint64_t)(PAGE_SIZE-1)))continue;
        }
        if(!advance){v->exit=HYP_EXIT_FAULT;return;}
        v->pc+=readvm(0x440c);
        if(v->vm->managed_console&&hyp_console_pending(v->vm)){v->exit=HYP_EXIT_YIELD;return;}
    }
    v->exit=HYP_EXIT_YIELD;
}
static int vmx_smp(hyp_vm_t *vm) { (void)vm; return 0; }
const hyp_backend_t *hyp_vmx_backend(void)
{
    static const hyp_backend_t b={
        .name="x86-vmx",.gpa_limit=1ULL<<48,.ram_base=0x100000,.max_running_vcpus=CONFIG_NR_CPUS,.probe_smp=vmx_smp,
        .reset=vmx_reset,.set_boot=vmx_boot,.vm_init=vmx_vm_init,.vm_destroy=hyp_s2_tree_destroy,
        .map=hyp_s2_tree_map,.pin=hyp_s2_tree_pin,.unmap=hyp_s2_tree_unmap,
        .translate=hyp_s2_tree_translate,.audit=hyp_s2_tree_audit,
        .load=vmx_load,.enter=vmx_enter,.put=vmx_put,.vcpu_destroy=vmx_destroy,
    };return &b;
}
