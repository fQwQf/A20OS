/* Opt-in hardware smoke: exercises real guest entry and lifecycle without a
 * rootfs. Enabled by the exact kernel argument a20.hyp_test=1. */
#include "hyp/hyp_vcpu.h"
#include "hyp/hyp_test.h"
#include "core/bootargs.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/smp.h"
#include "core/timer.h"
#include "proc/proc.h"
#include "cpu.h"
#include "mm/frame.h"

__attribute__((weak)) const hyp_test_payload_t *hyp_arch_test_payload(void)
{
    return NULL;
}

static int requested_arg(const char *key)
{
    const char *args = bootargs_get();
    if (!args) return 0;
    while (*args) {
        while (*args == ' ' || *args == '\t') args++;
        const char *end = args;
        while (*end && *end != ' ' && *end != '\t') end++;
        if ((size_t)(end - args) == strlen(key) && !memcmp(args, key, strlen(key))) return 1;
        args = end;
    }
    return 0;
}

static int protect_page(hyp_vm_t *vm, uint64_t gpa, pte_t prot)
{
    pfn_t p;
    int rc = vm->backend->pin(vm, gpa, &p);
    if (rc) return rc;
    rc = hyp_s2_unmap(vm, gpa);
    if (!rc) {
        rc = hyp_s2_map(vm, gpa, p, prot);
        if (rc) frame_put(p); /* unmapped ownership was not transferred */
    }
    frame_put(p); /* pin reference */
    return rc;
}

static int permission_test(const hyp_backend_t *b, const hyp_test_payload_t *p, int nx)
{
    hyp_vm_t *vm = hyp_vm_create(2ULL << 20);
    hyp_vcpu_t *v = NULL;
    int ok = 0;
    if (!vm) return 0;
    uint32_t zero = 0;
    if (hyp_vm_load(vm, b->ram_base, nx ? p->code : p->smp_code, nx ? p->code_size : p->smp_size)) goto out;
    if (nx) {
        if (protect_page(vm, b->ram_base, PTE_R | PTE_W)) goto out;
    } else {
        if (hyp_vm_load(vm, b->ram_base + PAGE_SIZE, &zero, sizeof(zero)) ||
            protect_page(vm, b->ram_base + PAGE_SIZE, PTE_R)) goto out;
    }
    v = hyp_vcpu_create(vm, b->ram_base);
    if (!v) goto out;
    if (!nx) b->reset(v, b->ram_base, 0, b->ram_base + PAGE_SIZE);
    int rc = 0;
    for (unsigned i = 0; i < 128; i++) {
        rc = hyp_vcpu_run(v);
        if (rc || v->exit != HYP_EXIT_YIELD) break;
    }
    ok = !rc && v->exit == HYP_EXIT_FAULT;
    if (!nx) {
        uint32_t value = 0;
        if (hyp_vm_copy_from_guest(vm, &value, b->ram_base + PAGE_SIZE, sizeof(value)) || value) ok = 0;
    }
out:
    if (v) hyp_vcpu_put(v);
    hyp_vm_put(vm);
    return ok;
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
    if (!permission_test(backend, payload, 1) ||
        (payload->smp_code && !permission_test(backend, payload, 0))) {
        printf("HYP_ARCH_TEST: FAIL stage2 protection\n");
        return;
    }
    hyp_stats_get(&after);
    if (before.vms != after.vms || before.vcpus != after.vcpus ||
        before.mapped_pages != after.mapped_pages || before.table_pages != after.table_pages) {
        printf("HYP_ARCH_TEST: FAIL protection accounting\n");
        return;
    }
    printf("HYP_ARCH_TEST: NX+RO PASS\n");
#ifdef ARCH_HAS_HYP_SBI
    if (payload->abi_selftest) {
        if (payload->abi_selftest()) return;
        hyp_stats_get(&after);
        if (before.vms != after.vms || before.vcpus != after.vcpus ||
            before.mapped_pages != after.mapped_pages || before.table_pages != after.table_pages) {
            printf("HYP_ARCH_TEST: FAIL guest ABI accounting\n");
            return;
        }
    }
#endif
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

/* Each worker owns a vCPU, is pinned to a distinct host CPU and runs the
 * same guest. The guest writes its own ready word and waits for the host's
 * release word; an observed peak of two RUN owners rules out serial tests. */
struct smp_test_worker {
    hyp_vcpu_t *vcpu;
    unsigned cpu, done, slices;
    int rc;
};
static struct smp_test_worker smp_workers[2];

static void smp_worker(unsigned id)
{
    arch_local_irq_enable();
    struct smp_test_worker *w = &smp_workers[id];
    w->rc = -EIO;
    if (cpu_current_id() != w->cpu) goto done;
    uint64_t limit = timer_get_ticks() + MS_TO_TICKS(10000);
    do {
        w->rc = hyp_vcpu_run(w->vcpu);
        w->slices++;
        if (w->rc || w->vcpu->exit != HYP_EXIT_YIELD) break;
        proc_yield();
    } while (timer_get_ticks() < limit);
done:
    __atomic_store_n(&w->done, 1, __ATOMIC_RELEASE);
    proc_exit(0);
}
static void smp_worker0(void) { smp_worker(0); }
static void smp_worker1(void) { smp_worker(1); }

/* Poll reaping with a bounded deadline. This boot diagnostic also runs on
 * software CPU models whose idle/wakeup timing differs from real hardware. */
static int reap_worker(int pid, int *status)
{
    uint64_t limit = timer_get_ticks() + MS_TO_TICKS(5000);
    do {
        int rc = proc_wait4(pid, status, 1); /* WNOHANG */
        if (rc) return rc;
        proc_yield();
    } while (timer_get_ticks() < limit);
    return -ETIMEDOUT;
}

static void smp_controller(void)
{
    arch_local_irq_enable();
    printf("HYP_SMP_TEST: begin\n");
    const hyp_backend_t *b = hyp_arch_backend();
    const hyp_test_payload_t *p = hyp_arch_test_payload();
    struct hyp_stats before, after;
    hyp_stats_get(&before);
    int ok = b && p && p->smp_code && b->reset;
    unsigned cpus[2] = {0}, found = 0;
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS && found < 2; cpu++)
        if (smp_cpu_is_online(cpu)) cpus[found++] = cpu;
    if (found < 2) ok = 0;
    hyp_vm_t *vm = ok ? hyp_vm_create(2ULL << 20) : NULL;
    hyp_vcpu_t *vcpu[2] = {NULL, NULL};
    task_t *tasks[2] = {NULL, NULL};
    int pids[2] = {-1, -1};
    unsigned peak = 0;
    const char *step = "configure";
    if (!vm || hyp_vm_configure_smp(vm, 2)) { ok = 0; goto out; }
    if (hyp_console_config(vm, 1)) { ok = 0; goto out; }
    step = "load";
    if (hyp_vm_load(vm, b->ram_base, p->smp_code, p->smp_size)) { ok = 0; goto out; }
    step = "vcpus";
    for (unsigned i = 0; i < 2; i++) {
        vcpu[i] = hyp_vcpu_create(vm, b->ram_base);
        if (!vcpu[i]) { ok = 0; goto out; }
        b->reset(vcpu[i], b->ram_base, i, b->ram_base + PAGE_SIZE + 4 * i);
    }
    if (hyp_vm_hart_start(vm, 1, b->ram_base, b->ram_base + PAGE_SIZE + 4)) {
        ok = 0; goto out;
    }
    step = "workers";
    for (unsigned i = 0; i < 2; i++) {
        memset(&smp_workers[i], 0, sizeof(smp_workers[i]));
        smp_workers[i].vcpu = vcpu[i];
        smp_workers[i].cpu = cpus[i];
        pids[i] = proc_alloc_deferred(i ? smp_worker1 : smp_worker0);
        if (pids[i] < 0) { ok = 0; goto out; }
        tasks[i] = proc_find_get(pids[i]);
        proc_sched_config_t cfg = { .fields = PROC_SCHED_AFFINITY,
                                   .affinity = 1u << cpus[i] };
        if (!tasks[i] || proc_sched_set(tasks[i], &cfg)) { ok = 0; goto out; }
        proc_publish_deferred_task(tasks[i]);
    }
    printf("HYP_SMP_TEST: workers published\n");
    step = "rendezvous";
    uint64_t deadline = timer_get_ticks() + MS_TO_TICKS(5000);
    uint32_t words[3] = {0};
    for (;;) {
        int rc = hyp_vm_copy_from_guest(vm, words, b->ram_base + PAGE_SIZE, sizeof(words));
        uint64_t flags = spin_lock_irqsave(&vm->control_lock);
        peak = vm->peak_run_count;
        spin_unlock_irqrestore(&vm->control_lock, flags);
        if (!rc && words[0] == 1 && words[1] == 1 && peak >= 2) break;
        if (timer_get_ticks() >= deadline ||
            __atomic_load_n(&smp_workers[0].done, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&smp_workers[1].done, __ATOMIC_ACQUIRE)) {
            printf("HYP_SMP_TEST: rendezvous timeout ready=%u,%u peak=%u done=%u,%u rc=%d,%d exit=%u,%u pc=%lx,%lx cause=%lx,%lx\n",
                   words[0], words[1], peak, smp_workers[0].done, smp_workers[1].done,
                   smp_workers[0].rc, smp_workers[1].rc, (unsigned)vcpu[0]->exit, (unsigned)vcpu[1]->exit,
                   (unsigned long)vcpu[0]->pc, (unsigned long)vcpu[1]->pc,
                   (unsigned long)vcpu[0]->exit_scause, (unsigned long)vcpu[1]->exit_scause);
            ok = 0; goto out;
        }
        proc_yield();
    }
    printf("HYP_SMP_TEST: ready peak=%u\n", peak);
    step = "release";
    uint32_t release = 1;
    if (hyp_vm_copy_to_guest(vm, b->ram_base + PAGE_SIZE + 8, &release, sizeof(release))) {
        ok = 0; goto out;
    }
    for (unsigned i = 0; i < 2; i++) {
        int status = -1;
        int reaped = reap_worker(pids[i], &status);
        if (reaped == pids[i]) pids[i] = -1;
        else ok = 0;
        if (status || smp_workers[i].rc || vcpu[i]->exit != HYP_EXIT_HART_STOPPED)
            ok = 0;
    }
#ifdef ARCH_HAS_HYP_SBI
    if (p->smp_check_offset) {
        uint32_t value = 0;
        if (hyp_vm_copy_from_guest(vm, &value, b->ram_base + p->smp_check_offset,
                                   sizeof(value)) || value != p->smp_check_value) {
            step = "guest-smp-check";
            ok = 0;
            goto out;
        }
        printf("HYP_SBI_SMP_TEST: PASS remote-ipi+rfence+hsm\n");
    }
#endif
    step = "stopped";
out:
    for (unsigned i = 0; i < 2; i++) if (vcpu[i]) hyp_vcpu_control(vcpu[i], HYP_CONTROL_STOP);
    for (unsigned i = 0; i < 2; i++) {
        if (pids[i] > 0 && tasks[i]) {
            proc_publish_deferred_task(tasks[i]);
            int status;
            if (reap_worker(pids[i], &status) != pids[i]) ok = 0;
        }
        if (tasks[i]) proc_put(tasks[i]);
        if (vcpu[i]) hyp_vcpu_put(vcpu[i]);
    }
    if (vm) hyp_vm_put(vm);
    hyp_stats_get(&after);
    if (before.vms != after.vms || before.vcpus != after.vcpus ||
        before.mapped_pages != after.mapped_pages || before.table_pages != after.table_pages) ok = 0;
    printf("HYP_SMP_TEST: %s backend=%s cpus=%u,%u peak=%u step=%s\n",
           ok ? "PASS" : "FAIL", b ? b->name : "none", cpus[0], cpus[1], peak, step);
    printf("part ok\nSystem is going down for power-off NOW\n");
    firmware_shutdown();
    proc_exit(ok ? 0 : 1);
}

void hyp_boot_selftest(void)
{
    if (!requested_arg("a20.hyp_test=1")) return;
    int enabled = arch_irqs_enabled();
    arch_local_irq_enable();
    run_selftest();
    if (requested_arg("a20.hyp_smp=1") && hyp_supported()) {
        if (proc_alloc(smp_controller) < 0) {
            printf("HYP_SMP_TEST: FAIL controller allocation\n");
        } else {
            sched();
            for (;;) arch_wfi();
        }
    }
    if (!enabled) arch_local_irq_disable();
}
