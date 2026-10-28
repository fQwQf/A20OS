#include "hyp/hyp_sbi.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static hyp_vm_t vm;
static hyp_vcpu_t v;
static uint8_t memory[3 * PAGE_SIZE], output[1024], input[1024];
static pte_t perms[3];
static unsigned in_head, in_tail, out_len, out_limit, refs, fills;
static uint64_t deadline, mask_seen, base_seen;
static unsigned irq_clears, syncs, local_fences, starts;
static int lifecycle_rc, fence_rc, deny_pin;
void *pfn_to_virt(pfn_t p) { assert(p < 3); return memory + p * PAGE_SIZE; }
void frame_put(pfn_t p) { (void)p; assert(refs); --refs; }
static int pin(hyp_vm_t *m, uint64_t gpa, pfn_t *out) {
    assert(m == &vm && gpa >= m->ram_base);
    unsigned p = (gpa - m->ram_base) / PAGE_SIZE;
    if (p >= 3 || !perms[p]) return -EFAULT;
    ++refs; *out = p; return 0;
}
pte_t hyp_riscv_s2_prot(hyp_vm_t *m, uint64_t gpa) {
    assert(m == &vm);
    if (gpa < m->ram_base || gpa - m->ram_base >= sizeof(memory)) return 0;
    return perms[(gpa - m->ram_base) / PAGE_SIZE];
}
int hyp_riscv_s2_pin_access(hyp_vm_t *m, uint64_t gpa, pte_t need, pfn_t *p) {
    if (deny_pin || (hyp_riscv_s2_prot(m, gpa) & need) != need) return -EACCES;
    return pin(m, gpa, p);
}
int hyp_ram_fill(hyp_vm_t *m, uint64_t gpa) {
    if (!hyp_vm_ram_contains(m, gpa)) return -EFAULT;
    unsigned p = (gpa - m->ram_base) / PAGE_SIZE;
    if (!perms[p]) { ++fills; perms[p] = PTE_V | PTE_R | PTE_W | PTE_X; }
    return 0;
}
int hyp_vm_ram_contains(hyp_vm_t *m, uint64_t gpa) {
    return gpa >= m->ram_base && gpa - m->ram_base < m->ram_size;
}
int hyp_console_putchar(hyp_vm_t *m, uint8_t c) {
    assert(m == &vm);
    if (out_len == out_limit) return -EAGAIN;
    output[out_len++] = c; return 0;
}
int hyp_console_getchar(hyp_vm_t *m) {
    assert(m == &vm);
    return in_tail == in_head ? -EAGAIN : input[in_tail++];
}
void hyp_arch_vstimecmp_set(uint64_t d) { deadline = d; }
int hyp_vcpu_inject_irq(hyp_vcpu_t *cpu, unsigned irq, int level) {
    assert(cpu == &v && irq == HYP_IRQ_TIMER && !level); ++irq_clears; return 0;
}
void hyp_vcpu_sync_irqs(hyp_vcpu_t *cpu) { assert(cpu == &v); ++syncs; }
void hyp_vm_shutdown(hyp_vm_t *m) { assert(m == &vm); m->shutdown = 1; }
int hyp_vm_hart_status(hyp_vm_t *m, uint64_t hart) {
    assert(m == &vm); return hart < 2 ? (int)hart : -EINVAL;
}
int hyp_vm_hart_start(hyp_vm_t *m, uint64_t hart, uint64_t entry, uint64_t opaque) {
    assert(m == &vm && hart < 2 && !(entry & 1)); (void)opaque;
    ++starts; return lifecycle_rc;
}
int hyp_vm_ipi(hyp_vm_t *m, uint64_t mask, uint64_t base) {
    assert(m == &vm); mask_seen = mask; base_seen = base; return fence_rc;
}
int hyp_vm_fence(hyp_vm_t *m, uint64_t mask, uint64_t base, unsigned kind) {
    assert(m == &vm && kind <= 1); return hyp_vm_ipi(m, mask, base);
}
void hyp_riscv_local_fence(unsigned kind) { assert(kind <= 1); ++local_fences; }

static void setup(void) {
    memset(&vm, 0, sizeof(vm)); memset(&v, 0, sizeof(v));
    memset(perms, 0, sizeof(perms)); memset(memory, 0, sizeof(memory));
    static const hyp_backend_t b = { .pin = pin };
    vm.backend = &b; vm.ram_base = 0x80000000; vm.ram_size = sizeof(memory);
    v.vm = &vm; v.pc = vm.ram_base; out_limit = sizeof(output);
    in_head = in_tail = out_len = refs = fills = 0;
    irq_clears = syncs = local_fences = starts = 0;
    lifecycle_rc = fence_rc = deny_pin = 0;
}
static int call(uint32_t eid, uint32_t fid, uint64_t a0, uint64_t a1, uint64_t a2) {
    for (unsigned i = 0; i < 32; i++) v.regs[i] = (reg_t)(0x12340000 + i);
    v.regs[10] = (reg_t)a0; v.regs[11] = (reg_t)a1; v.regs[12] = (reg_t)a2;
    v.regs[17] = eid; v.regs[16] = fid;
    uint64_t saved[32]; memcpy(saved, v.regs, sizeof(saved));
    uint64_t pc = v.pc;
    int rc = hyp_riscv_sbi_ecall(&v, 10);
    for (unsigned i = 0; i < 32; i++) {
        if (i == 10 || (i == 11 && eid >= 0x10)) continue;
        assert(v.regs[i] == saved[i]);
    }
    if (rc) assert(v.pc == (reg_t)(pc + 4));
    else assert(v.pc == pc);
    assert(!refs);
    return rc;
}
static void expect(int err) { assert((reg_t)v.regs[10] == (reg_t)err); }
int main(void) {
    setup();
    for (unsigned fid = 0; fid <= 6; fid++) {
        assert(call(HYP_SBI_BASE, fid, HYP_SBI_DBCN, 0, 0)); expect(0);
        if (!fid) assert(v.regs[11] == HYP_SBI_SPEC_VERSION);
        if (fid == 1) assert(v.regs[11] == HYP_SBI_IMPL_ID);
        if (fid == 3) assert(v.regs[11] == 1);
    }
    const unsigned extensions[] = {0,1,2,8,HYP_SBI_BASE,HYP_SBI_TIME,HYP_SBI_HSM,
        HYP_SBI_IPI,HYP_SBI_RFENCE,HYP_SBI_SRST,HYP_SBI_DBCN};
    for (unsigned i = 0; i < sizeof(extensions)/sizeof(*extensions); i++) {
        call(HYP_SBI_BASE, 3, extensions[i], 0, 0); expect(0); assert(v.regs[11] == 1);
    }
    call(HYP_SBI_BASE, 3, 0x504d55, 0, 0); expect(0); assert(!v.regs[11]);
    v.pc = (reg_t)-4;
    call(0x7fffffff, 0, 0, 0, 0); expect(-2); assert(v.pc == 0);
    call(0x7fffffff, 0, 0, 0, 0); expect(-2);
    call(HYP_SBI_BASE, 7, 0, 0, 0); expect(-2);
    call(HYP_SBI_TIME, 99, 0, 0, 0); expect(-2);
    for (unsigned i = 4; i < sizeof(extensions)/sizeof(*extensions); i++) {
        call(extensions[i], 0xffffffffu, 0, 0, 0); expect(-2);
    }
#if defined(CONFIG_64BIT)
    call(HYP_SBI_BASE, 3, (1ULL << 32) | HYP_SBI_TIME, 0, 0);
    expect(0); assert(v.regs[11] == 1);
#endif
    call(3, 999, 0, 123, 0); expect(-2); assert(v.regs[11] == 123);
    call(2, 999, 0, 123, 0); expect(-1);
    input[in_head++] = 0xff;
    call(2, 0, 0, 123, 0); expect(255);
    call(0, 999, 0x89abcdef, 0x12345678, 0); expect(0);
#if defined(CONFIG_32BIT)
    assert(deadline == 0x1234567889abcdefULL);
#else
    assert(deadline == 0x89abcdef);
#endif
    call(HYP_SBI_TIME, 0, (reg_t)-1, (reg_t)-1, 0); expect(0);
    assert(deadline == (uint64_t)-1 && v.timer_deadline == deadline);
    assert(irq_clears == 2 && syncs == 2);
    call(HYP_SBI_HSM, 0, 99, vm.ram_base, 0); expect(-3);
    call(HYP_SBI_HSM, 0, 1, vm.ram_base + 1, 0); expect(-5);
    call(HYP_SBI_HSM, 0, 1, vm.ram_base - 2, 0); expect(-5);
    perms[0] = PTE_V | PTE_R;
    call(HYP_SBI_HSM, 0, 1, vm.ram_base, 0); expect(-5); assert(!starts);
    perms[0] |= PTE_X;
    call(HYP_SBI_HSM, 0, 1, vm.ram_base, 0); expect(0); assert(starts == 1);
    lifecycle_rc = -EALREADY;
    call(HYP_SBI_HSM, 0, 1, vm.ram_base, 0); expect(-6);
    call(HYP_SBI_HSM, 2, 1, 0, 0); expect(0); assert(v.regs[11] == 1);
    call(HYP_SBI_HSM, 3, 0, 0, 0); expect(-2);
    assert(!call(HYP_SBI_HSM, 1, 0, 0, 0));
    assert(v.exit == HYP_EXIT_HART_STOPPED && v.hart_state == HYP_HART_STOP_PENDING);
    v.exit = HYP_EXIT_NONE;
    call(HYP_SBI_IPI, 0, 3, (reg_t)-1, 0); expect(0); assert(base_seen == (uint64_t)-1);
    call(HYP_SBI_RFENCE, 0, 1, 0, 0); expect(0); assert(mask_seen == 1 && local_fences == 1);
    fence_rc = -EINVAL;
    call(HYP_SBI_RFENCE, 2, 4, 0, 0); expect(-3); assert(local_fences == 1);
    call(HYP_SBI_SRST, 0, 0, 2, 0); expect(-3); assert(!vm.shutdown);
    call(HYP_SBI_SRST, 0, 3, 0, 0); expect(-3);
    call(HYP_SBI_SRST, 0, 1, 0, 0); expect(-2);
    call(HYP_SBI_SRST, 1, 0, 0, 0); expect(-2);

    setup();
    call(HYP_SBI_DBCN, 0, 2, vm.ram_base - 1, 0); expect(-3); assert(!out_len && !fills);
    call(HYP_SBI_DBCN, 0, 1, (reg_t)-1, (reg_t)-1); expect(-3);
    call(HYP_SBI_DBCN, 0, HYP_SBI_CONSOLE_MAX_BUFFER+1, vm.ram_base, 0); expect(-3);
    perms[0] = PTE_V | PTE_R | PTE_X;
    call(HYP_SBI_DBCN, 1, 1, vm.ram_base, 0); expect(-3);
    assert(!in_tail && !fills);
    /* Denied second page is checked before consuming even first-page input. */
    perms[0] |= PTE_W; perms[1] = PTE_V | PTE_R;
    call(HYP_SBI_DBCN, 1, 2, vm.ram_base + PAGE_SIZE - 1, 0); expect(-3);
    perms[1] |= PTE_W;
    input[in_head++] = 'R'; input[in_head++] = 'V';
    call(HYP_SBI_DBCN, 1, 2, vm.ram_base + PAGE_SIZE - 1, 0); expect(0);
    assert(v.regs[11] == 2 && memory[PAGE_SIZE-1] == 'R' && memory[PAGE_SIZE] == 'V');
    call(HYP_SBI_DBCN, 1, 2, vm.ram_base, 0); expect(0); assert(!v.regs[11]);
    deny_pin = 1;
    unsigned consumed = in_tail;
    call(HYP_SBI_DBCN, 1, 2, vm.ram_base, 0); expect(-3);
    assert(in_tail == consumed && !out_len);
    deny_pin = 0;
    out_limit = 1;
    call(HYP_SBI_DBCN, 0, 2, vm.ram_base + PAGE_SIZE - 1, 0); expect(0);
    assert(v.regs[11] == 1 && out_len == 1 && output[0] == 'R');
    uint64_t pc = v.pc;
    assert(!call(HYP_SBI_DBCN, 2, 'X', 0, 0));
    assert(v.exit == HYP_EXIT_YIELD && v.pc == pc && v.regs[10] == 'X');
    assert(!call(1, 99, 'L', 123, 0));
    assert(v.regs[10] == 'L' && v.regs[11] == 123);
    out_limit = sizeof(output);
    assert(call(HYP_SBI_DBCN, 2, 'X', 0, 0)); expect(0); assert(output[1] == 'X');
    call(HYP_SBI_DBCN, 0, 512, vm.ram_base, 0); expect(0); assert(v.regs[11] == 256);
    call(HYP_SBI_DBCN, 0, 1, vm.ram_base + 2*PAGE_SIZE, 0); expect(0); assert(fills == 1);
#if defined(CONFIG_32BIT)
    /* DBCN physical addresses are 2*XLEN, not truncated to RV32 pointers. */
    vm.ram_base += 1ULL << 32;
    call(HYP_SBI_DBCN, 0, 1, (uint32_t)vm.ram_base, vm.ram_base >> 32);
    expect(0); assert(v.regs[11] == 1);
#endif
    assert(!call(HYP_SBI_SRST, 0, 0, 1, 0));
    assert(vm.shutdown && v.exit == HYP_EXIT_SHUTDOWN);
    puts("virtual SBI dispatcher: PASS");
    return 0;
}
