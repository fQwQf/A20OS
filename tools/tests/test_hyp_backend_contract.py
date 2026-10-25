#!/usr/bin/env python3
"""Run production hypervisor availability and VM admission code on the host.

The architecture stubs and weak fallback are compiled unchanged, using the real
hypervisor headers. Only MM/lock/log platform dependencies are shimmed. The VM
admission fixture extracts hyp_vm_create, mocks allocation and backend callbacks,
and checks that malformed or unavailable backends cannot publish a VM. These
checks do not claim hardware guest execution coverage.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
UNAVAILABLE_ARCHES = ("arm32", "riscv32", "loongarch64", "ppc64le")

SHIMS = {
    "mm/pt.h": """#pragma once
#include "core/types.h"
typedef uint32_t pfn_t;
typedef struct { unsigned errors; } mm_pt_audit_report_t;
#define PAGE_SIZE 4096u
""",
    "core/lock.h": """#pragma once
#include "core/types.h"
typedef struct { unsigned state; } spinlock_t;
static inline void spin_init(spinlock_t *lock) { lock->state = 0; }
""",
    "core/klog.h": """#pragma once
void test_log(const char *fmt, ...);
#define kerr(...) test_log(__VA_ARGS__)
""",
}

UNAVAILABLE_HARNESS = r'''
#include "hyp/hyp_vcpu.h"
#include "core/errno.h"
#include <assert.h>
#include <string.h>
static unsigned log_count;
void test_log(const char *fmt, ...) {
    assert(strstr(fmt, "unavailable"));
    ++log_count;
}
int main(void) {
    hyp_vcpu_t guest, before;
    memset(&guest, 0xa5, sizeof(guest));
    memcpy(&before, &guest, sizeof(guest));
    assert(hyp_supported() == 0);
    assert(hyp_arch_backend() == NULL);
    assert(hyp_arch_vcpu_setup(NULL) == -EOPNOTSUPP);
    assert(hyp_arch_vcpu_setup(&guest) == -EOPNOTSUPP);
    assert(memcmp(&guest, &before, sizeof(guest)) == 0);
    hyp_arch_vsatp_set(~(uint64_t)0);
    assert(hyp_arch_vsatp_get() == 0);
    hyp_arch_vstimecmp_set(~(uint64_t)0);
    assert(hyp_arch_vstimecmp_get() == 0);
    hyp_arch_vmid_fenced(65535);
    hyp_arch_s2_fence(65535, ~(uint64_t)0);
    hyp_arch_host_tlb_fence();
    hyp_arch_vcpu_enter(&guest);
    assert(log_count == 1);
    assert(memcmp(&guest, &before, sizeof(guest)) == 0);
    assert(hyp_arch_backend() == NULL);
    return 0;
}
'''

ADMISSION_PRELUDE = r'''
#include "hyp/hyp.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static uint64_t hyp_live_vms;
static int supported = 1, init_result, init_calls;
static unsigned allocations, frees;
static const hyp_backend_t *selected;
static struct cg_node *owner_group;
typedef struct { struct cg_node *cgroup; } task_t;
static task_t owner;
int hyp_supported(void) { return supported; }
const hyp_backend_t *hyp_arch_backend(void) { return selected; }
static task_t *proc_current(void) { return &owner; }
static void *kcalloc(size_t count, size_t size) {
    ++allocations;
    return calloc(count, size);
}
static void kfree(void *ptr) { ++frees; free(ptr); }
static int vm_init(struct hyp_vm *vm) {
    assert(vm->backend == selected);
    assert(vm->charge_cg == owner_group);
    assert(vm->magic == 0); /* unpublished until init succeeds */
    ++init_calls;
    return init_result;
}
static void vm_destroy(struct hyp_vm *vm) { (void)vm; }
static int map(struct hyp_vm *vm, uint64_t gpa, pfn_t pfn, pte_t prot) {
    (void)vm; (void)gpa; (void)pfn; (void)prot; return 0;
}
static int unmap(struct hyp_vm *vm, uint64_t gpa) {
    (void)vm; (void)gpa; return 0;
}
static paddr_t translate(struct hyp_vm *vm, uint64_t gpa) {
    (void)vm; (void)gpa; return 0;
}
static int load(struct hyp_vcpu *vcpu) { (void)vcpu; return 0; }
static void enter(struct hyp_vcpu *vcpu) { (void)vcpu; }
static void put(struct hyp_vcpu *vcpu) { (void)vcpu; }
'''

ADMISSION_HARNESS = r'''
static hyp_backend_t complete(void) {
    hyp_backend_t b = {
        .name = "host-test", .gpa_limit = 1ULL << 40, .ram_base = 0x40000000,
        .vm_init = vm_init, .vm_destroy = vm_destroy,
        .map = map, .unmap = unmap, .translate = translate,
        .load = load, .enter = enter, .put = put,
    };
    return b;
}
static void rejected(uint64_t size) {
    unsigned before_allocations = allocations;
    unsigned before_frees = frees;
    int before_init = init_calls;
    uint64_t before_vms = hyp_live_vms;
    assert(hyp_vm_create(size) == NULL);
    assert(allocations == before_allocations);
    assert(frees == before_frees);
    assert(init_calls == before_init);
    assert(hyp_live_vms == before_vms);
}
#define CHECK_MISSING(member) do { \
    b = complete(); b.member = NULL; rejected(PAGE_SIZE); \
} while (0)
int main(void) {
    hyp_backend_t b = complete();
    selected = &b;
    supported = 0;
    rejected(PAGE_SIZE); /* backend cannot bypass unavailable hardware */
    supported = 1;
    selected = NULL;
    rejected(PAGE_SIZE); /* a CPU feature cannot bypass a missing backend */
    selected = &b;
    CHECK_MISSING(vm_init);
    CHECK_MISSING(vm_destroy);
    CHECK_MISSING(map);
    CHECK_MISSING(unmap);
    CHECK_MISSING(translate);
    CHECK_MISSING(load);
    CHECK_MISSING(enter);
    CHECK_MISSING(put);
    b = complete();
    rejected(0);
    rejected(PAGE_SIZE - 1);
    rejected(PAGE_SIZE + 1);
    rejected(b.gpa_limit - b.ram_base + PAGE_SIZE);
    rejected(~(uint64_t)(PAGE_SIZE - 1)); /* subtraction check, no overflow */
    b.ram_base = b.gpa_limit;
    rejected(PAGE_SIZE);
    b.ram_base = ~(uint64_t)0 - (PAGE_SIZE - 1);
    rejected(PAGE_SIZE);
    b = complete();
    owner_group = (struct cg_node *)(uintptr_t)0x1234;
    owner.cgroup = owner_group;
    init_result = -1;
    assert(hyp_vm_create(PAGE_SIZE) == NULL);
    assert(allocations == 1 && frees == 1 && init_calls == 1);
    assert(hyp_live_vms == 0);
    init_result = 0;
    hyp_vm_t *vm = hyp_vm_create(PAGE_SIZE);
    assert(vm != NULL);
    assert(vm->magic == HYP_VM_MAGIC && vm->refcount == 1);
    assert(vm->backend == &b && vm->ram_base == b.ram_base);
    assert(vm->mem_size == PAGE_SIZE && vm->ram_size == PAGE_SIZE);
    assert(vm->vcpu_limit == 1 && vm->parallel_limit == 1);
    assert(hyp_live_vms == 1 && allocations == 2 && init_calls == 2);
    free(vm);
    return 0;
}
'''


class HypervisorBackendContractTest(unittest.TestCase):
    def build_run(self, source, production=(), bits=64):
        with tempfile.TemporaryDirectory(prefix="a20-hyp-contract-") as tmp:
            tmp = Path(tmp)
            for name, content in SHIMS.items():
                path = tmp / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            harness = tmp / "main.c"
            harness.write_text(source)
            binary = tmp / "test"
            subprocess.run([
                "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-fno-builtin", f"-DCONFIG_{bits}BIT=1", "-I", str(tmp),
                "-I", str(ROOT / "kernel/include"),
                *(str(ROOT / path) for path in production), str(harness),
                "-o", str(binary),
            ], check=True, capture_output=True, text=True)
            subprocess.run([str(binary)], check=True, capture_output=True, text=True)

    def test_unavailable_architectures_fail_without_guest_state_changes(self):
        for arch in UNAVAILABLE_ARCHES:
            with self.subTest(arch=arch):
                self.build_run(UNAVAILABLE_HARNESS, (
                    f"kernel/arch/{arch}/hyp/hyp_arch.c",
                    "kernel/hyp/hyp_backend.c",
                ), bits=32 if arch in ("arm32", "riscv32") else 64)

    def test_weak_fallback_has_no_backend(self):
        self.build_run('''#include "hyp/hyp_backend.h"
#include <assert.h>
int main(void) { assert(hyp_arch_backend() == NULL); return 0; }
''', ("kernel/hyp/hyp_backend.c",))

    def test_vm_admission_requires_complete_backend_before_allocation(self):
        source = (ROOT / "kernel/hyp/hyp.c").read_text()
        match = re.search(r"^hyp_vm_t \*hyp_vm_create\([^)]*\)\n\{.*?^\}",
                          source, re.MULTILINE | re.DOTALL)
        self.assertIsNotNone(match, "production hyp_vm_create not found")
        self.build_run(ADMISSION_PRELUDE + match.group() + ADMISSION_HARNESS)


if __name__ == "__main__":
    unittest.main()
