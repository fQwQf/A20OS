/* ISA-specific executable inputs for the common hardware smoke. */
#include "hyp/hyp_test.h"
#include "hyp/hyp_vcpu.h"
static const uint32_t code[] = {
    0x04800513, 0x00100893, 0x00000073,
    0x05900513, 0x00100893, 0x00000073,
    0x05000513, 0x00100893, 0x00000073,
    0x00a00513, 0x00100893, 0x00000073,
    0x00800893, 0x00000073,
};

extern const uint8_t hyp_sbi_smp_start[], hyp_sbi_smp_end[];
extern int hyp_arch_sbi_selftest(void);
static int register_restore_selftest(void)
{
    if (hyp_arch_sbi_selftest()) return -EIO;
    /* Real VS instructions store t0 before and after a console-induced RUN
     * exit. Its sentinel must survive both initial entry and re-entry; sp is
     * deliberately different so the historical temporary-register bug fails. */
    static const uint32_t guest[] = {
        0x0055b023, 0x05400513, 0x00100893, 0x00000073,
        0x0055b423, 0x00800893, 0x00000073,
    };
    const uint64_t sentinel = 0xa20b5a5a01234567ULL;
    uint64_t saved[2] = {0, 0};
    hyp_vm_t *vm = hyp_vm_create(2ULL << 20);
    hyp_vcpu_t *v = NULL;
    int rc = -ENOMEM;
    unsigned console_bytes = 0, runs = 0;
    if (!vm) goto out;
    uint64_t base = vm->ram_base;
    if (hyp_console_config(vm, 1) ||
        hyp_vm_load(vm, base, guest, sizeof(guest)) ||
        hyp_vm_load(vm, base + PAGE_SIZE, saved, sizeof(saved))) goto out;
    v = hyp_vcpu_create(vm, base);
    if (!v) goto out;
    vm->backend->reset(v, base, 0, base + PAGE_SIZE);
    v->regs[5] = sentinel;
    v->regs[2] = base + 2 * PAGE_SIZE - 16;
    for (; runs < 128; runs++) {
        rc = hyp_vcpu_run(v);
        uint8_t output[8];
        int count = hyp_console_read(vm, output, sizeof(output));
        if (count > 0) console_bytes += (unsigned)count;
        if (rc || v->exit != HYP_EXIT_YIELD) break;
    }
    if (!rc) rc = hyp_vm_copy_from_guest(vm, saved, base + PAGE_SIZE, sizeof(saved));
    if (!rc && (v->exit != HYP_EXIT_SHUTDOWN || runs < 1 ||
                console_bytes != 1 || saved[0] != sentinel || saved[1] != sentinel))
        rc = -EIO;
out:
    printf("HYP_GPR_TEST: %s t0 initial=%lx reentry=%lx\n",
           rc ? "FAIL" : "PASS", (unsigned long)saved[0], (unsigned long)saved[1]);
    if (v) hyp_vcpu_put(v);
    if (vm) hyp_vm_put(vm);
    return rc;
}
const hyp_test_payload_t *hyp_arch_test_payload(void)
{
    static hyp_test_payload_t p = {
        .code = code, .code_size = sizeof(code),
        .smp_check_offset = 0x100c, .smp_check_value = 0x51b1,
        .abi_selftest = register_restore_selftest,
    };
    /* Used serially by opt-in boot diagnostics before workers are published. */
    p.smp_code = hyp_sbi_smp_start;
    p.smp_size = (size_t)(hyp_sbi_smp_end - hyp_sbi_smp_start);
    return &p;
}
