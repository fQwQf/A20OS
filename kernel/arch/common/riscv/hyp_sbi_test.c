/* Opt-in real VS ECALL verification, called by the common boot smoke. */
#include "hyp/hyp_sbi.h"
#include "core/string.h"
#include "core/stdio.h"
#include "mm/frame.h"
extern const uint8_t hyp_sbi_test_start[], hyp_sbi_test_end[];

static int protect(hyp_vm_t *vm, uint64_t gpa, pte_t prot)
{
    pfn_t p;
    int rc = vm->backend->pin(vm, gpa, &p);
    if (rc) return rc;
    rc = hyp_s2_unmap(vm, gpa);
    if (!rc) {
        rc = hyp_s2_map(vm, gpa, p, prot);
        if (rc) frame_put(p);
    }
    frame_put(p);
    return rc;
}
int hyp_arch_sbi_selftest(void)
{
    hyp_vm_t *vm = hyp_vm_create(2ULL << 20);
    hyp_vcpu_t *v = NULL;
    uint64_t base = vm ? vm->ram_base : 0;
    uint8_t data[40] = {0};
    uint8_t output[8] = {0};
    unsigned used = 0;
    uint32_t report[2] = {0};
    int rc = -ENOMEM;
    if (!vm) goto out;
    size_t code_size = (size_t)(hyp_sbi_test_end - hyp_sbi_test_start);
    memcpy(data + 32, "SBI", 3);
    if (code_size > PAGE_SIZE || hyp_console_config(vm, 1) ||
        hyp_vm_load(vm, base, hyp_sbi_test_start, code_size) ||
        hyp_vm_load(vm, base + PAGE_SIZE, data, sizeof(data)) ||
        hyp_ram_fill(vm, base + 2 * PAGE_SIZE) ||
        protect(vm, base, PTE_R | HYP_PROT_EXEC) ||
        protect(vm, base + PAGE_SIZE, PTE_R | PTE_W) ||
        hyp_console_write(vm, (const uint8_t *)"rv", 2) != 2)
        goto out;
    v = hyp_vcpu_create(vm, base);
    if (!v) goto out;
    vm->backend->reset(v, base, 0, base + PAGE_SIZE);
    /* Ensure TIME clears a pending software-injected virtual timer IRQ. */
    v->hvip = (uint64_t)1 << 6;
    for (unsigned i = 0; i < 256; i++) {
        rc = hyp_vcpu_run(v);
        int n = hyp_console_read(vm, output + used, sizeof(output) - used);
        if (n > 0) used += (unsigned)n;
        if (rc || v->exit != HYP_EXIT_YIELD) break;
    }
    if (hyp_vm_copy_from_guest(vm, report, base + PAGE_SIZE, sizeof(report))) rc = -EFAULT;
    if (!rc && (v->exit != HYP_EXIT_SHUTDOWN || report[0] != 0x51b1 ||
                used != 4 || memcmp(output, "SBI!", 4))) rc = -EIO;
out:
    if (rc) printf("HYP_SBI_TEST: FAIL check=%u rc=%d exit=%u cause=%lx pc=%lx\n",
                   report[0], rc, v ? (unsigned)v->exit : 0,
                   v ? (unsigned long)v->exit_scause : 0,
                   v ? (unsigned long)v->pc : 0);
    else printf("HYP_SBI_TEST: PASS xlen=%u checks=%u\n", (unsigned)(8 * sizeof(reg_t)), report[1]);
    if (v) hyp_vcpu_put(v);
    if (vm) hyp_vm_put(vm);
    return rc;
}
