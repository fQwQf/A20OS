/* Opt-in hardware smoke: exercises real guest entry and lifecycle without a
 * rootfs. Enabled by the exact kernel argument a20.hyp_test=1. */
#include "hyp/hyp_vcpu.h"
#include "hyp/hyp_test.h"
#include "core/bootargs.h"
#include "core/string.h"
#include "core/stdio.h"

__attribute__((weak)) const hyp_test_payload_t *hyp_arch_test_payload(void)
{
    return NULL;
}

static int requested(void)
{
    const char *args = bootargs_get();
    const char key[] = "a20.hyp_test=1";
    if (!args) return 0;
    while (*args) {
        while (*args == ' ' || *args == '\t') args++;
        const char *end = args;
        while (*end && *end != ' ' && *end != '\t') end++;
        if ((size_t)(end - args) == sizeof(key) - 1 && !memcmp(args, key, sizeof(key) - 1)) return 1;
        args = end;
    }
    return 0;
}

static void run_selftest(void)
{
    if (!hyp_supported()) {
        printf("HYP_ARCH_TEST: SKIP (backend unavailable)\n");
        return;
    }
    const hyp_backend_t *backend = hyp_arch_backend();
    if (!backend || hyp_selftest()) {
        printf("HYP_ARCH_TEST: FAIL stage2\n");
        return;
    }
    const hyp_test_payload_t *payload = hyp_arch_test_payload();
    if (!payload) {
        printf("HYP_ARCH_TEST: SKIP (no test payload)\n");
        return;
    }
    struct hyp_stats before, after;
    hyp_stats_get(&before);
    for (unsigned i = 0; i < 3; i++) {
        const char *step = "create";
        hyp_vm_t *vm = hyp_vm_create(2ULL << 20);
        hyp_vcpu_t *vcpu = NULL;
        int rc = -ENOMEM;
        if (!vm) goto fail;
        step = "load";
        rc = hyp_vm_load(vm, backend->ram_base, payload->code, payload->code_size);
        if (rc) goto fail;
        step = "vcpu";
        vcpu = hyp_vcpu_create(vm, backend->ram_base);
        if (!vcpu) goto fail;
        step = "run";
        for (unsigned tries = 0; tries < 128; tries++) {
            rc = hyp_vcpu_run(vcpu);
            if (rc || vcpu->exit != HYP_EXIT_YIELD) break;
        }
        if (rc || vcpu->exit != HYP_EXIT_SHUTDOWN || hyp_vm_console_bytes(vm) != 4) goto fail;
        if (payload->check_offset) {
            step = "demand-fill";
            uint32_t value = 0;
            rc = hyp_vm_copy_from_guest(vm, &value, backend->ram_base + payload->check_offset, sizeof(value));
            if (rc || value != payload->check_value) goto fail;
        }
        hyp_vcpu_put(vcpu);
        hyp_vm_put(vm);
        hyp_stats_get(&after);
        if (before.vms != after.vms || before.vcpus != after.vcpus ||
            before.mapped_pages != after.mapped_pages || before.table_pages != after.table_pages) {
            printf("HYP_ARCH_TEST: FAIL accounting round=%u\n", i);
            return;
        }
        continue;
fail:
        printf("HYP_ARCH_TEST: FAIL %s round=%u rc=%d exit=%u cause=%lx pc=%lx\n",
               step, i, rc, vcpu ? (unsigned)vcpu->exit : 0,
               vcpu ? (unsigned long)vcpu->exit_scause : 0,
               vcpu ? (unsigned long)vcpu->pc : 0);
        if (vcpu) hyp_vcpu_put(vcpu);
        if (vm) hyp_vm_put(vm);
        return;
    }
    /* No voluntary traps: the host timer must preempt a masked guest. */
    if (payload->spin) {
        hyp_vm_t *spin_vm = hyp_vm_create(2ULL << 20);
        hyp_vcpu_t *spin_cpu = NULL;
        int spin_rc = spin_vm ? hyp_vm_load(spin_vm, backend->ram_base, payload->spin, payload->spin_size) : -ENOMEM;
        if (!spin_rc) spin_cpu = hyp_vcpu_create(spin_vm, backend->ram_base);
        if (!spin_cpu) spin_rc = -ENOMEM;
        if (!spin_rc) spin_rc = hyp_vcpu_run(spin_cpu);
        int preempted = !spin_rc && spin_cpu->exit == HYP_EXIT_YIELD;
        if (preempted) {
            hyp_vcpu_control(spin_cpu, HYP_CONTROL_STOP);
            spin_rc = hyp_vcpu_run(spin_cpu);
            preempted = !spin_rc && spin_cpu->exit == HYP_EXIT_STOPPED;
        }
        if (spin_cpu) hyp_vcpu_put(spin_cpu);
        if (spin_vm) hyp_vm_put(spin_vm);
        hyp_stats_get(&after);
        if (!preempted || before.vms != after.vms || before.vcpus != after.vcpus ||
            before.mapped_pages != after.mapped_pages || before.table_pages != after.table_pages) {
            printf("HYP_ARCH_TEST: FAIL preemption rc=%d\n", spin_rc);
            return;
        }
        printf("HYP_ARCH_TEST: preemption+stop PASS\n");
    }
    printf("HYP_ARCH_TEST: PASS backend=%s rounds=3\n", backend->name);
}

void hyp_boot_selftest(void)
{
    if (!requested()) return;
    int enabled = arch_irqs_enabled();
    arch_local_irq_enable();
    run_selftest();
    if (!enabled) arch_local_irq_disable();
}
