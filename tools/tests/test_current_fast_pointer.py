"""Host coverage for proc_current's architecture-pointer fast path.

This exercises the real C control flow with a mocked architecture pointer. It
cannot validate the hardware RDMSR/SWAPGS behavior of x86_64.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def extract_current(source: str) -> str:
    signature = "task_t *proc_current(void)"
    if source.count(signature) != 1:
        raise AssertionError(f"expected one definition of {signature!r}")
    start = source.index(signature)
    brace = source.find("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[start:pos + 1]
    raise AssertionError("unterminated proc_current definition")


HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#define ARCH_FAST_CURRENT 1
#define CONFIG_NR_CPUS @NR_CPUS@
typedef struct task { int id; } task_t;
static task_t fast_task = {10};
static task_t slot0 = {20}, slot1 = {21};
static task_t *g_cpu_current[2] = {&slot0, &slot1};
static task_t *arch_pointer;
static int irq_enabled = 1;
static unsigned selected_cpu = 1;
static unsigned cpu_reads, irq_disables, irq_enables;
static task_t *arch_get_task_pointer(void) { return arch_pointer; }
static int arch_is_kernel_address(const void *p) { return p == &fast_task; }
int arch_irqs_enabled(void) { return irq_enabled; }
void arch_local_irq_disable(void) { irq_disables++; irq_enabled = 0; }
void arch_local_irq_enable(void) { irq_enables++; irq_enabled = 1; }
unsigned cpu_current_id(void) { cpu_reads++; return selected_cpu; }
@PROC_CURRENT@
int main(void) {
    /* The active kernel stack pointer wins even while the remote-observer slot
     * has already been published as another task. */
    arch_pointer = &fast_task;
    irq_enabled = 1;
    assert(proc_current() == &fast_task);
    assert(cpu_reads == 0 && irq_disables == 0 && irq_enables == 0 && irq_enabled);

    /* A null or non-kernel architecture pointer uses the early/fallback slot. */
    arch_pointer = 0;
    selected_cpu = 1;
#if CONFIG_NR_CPUS == 1
    assert(proc_current() == &slot0);
#else
    assert(proc_current() == &slot1);
#endif
    arch_pointer = (task_t *)(uintptr_t)0x1234;
#if CONFIG_NR_CPUS == 1
    assert(proc_current() == &slot0);
#else
    assert(proc_current() == &slot1);
#endif
#if CONFIG_NR_CPUS == 1
    /* Single-CPU fallback reads slot zero directly and preserves IRQ state. */
    assert(cpu_reads == 0 && irq_disables == 0 && irq_enables == 0 && irq_enabled);
    irq_enabled = 0;
    assert(proc_current() == &slot0);
    assert(!irq_enabled && irq_disables == 0 && irq_enables == 0);
#else
    /* SMP fallback pins CPU-id plus slot lookup and restores the input state. */
    assert(cpu_reads == 2 && irq_disables == 2 && irq_enables == 2 && irq_enabled);
    irq_enabled = 0;
    arch_pointer = 0;
    assert(proc_current() == &slot1);
    assert(!irq_enabled && irq_disables == 3 && irq_enables == 2);
#endif
    return 0;
}
'''


class CurrentFastPointerTests(unittest.TestCase):
    def test_fast_pointer_and_fallback_for_one_and_two_cpus(self):
        source = (ROOT / "kernel/proc/current.c").read_text()
        body = extract_current(source)
        for nr_cpus in (1, 2):
            with self.subTest(nr_cpus=nr_cpus), tempfile.TemporaryDirectory(
                prefix=f"a20-current-fast-{nr_cpus}-"
            ) as directory:
                tmp = Path(directory)
                c_file, exe = tmp / "current_fast.c", tmp / "current_fast"
                c_file.write_text(
                    HARNESS.replace("@NR_CPUS@", str(nr_cpus)).replace("@PROC_CURRENT@", body)
                )
                subprocess.run(
                    ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                     str(c_file), "-o", str(exe)],
                    check=True,
                    cwd=ROOT,
                )
                subprocess.run([str(exe)], check=True, cwd=ROOT)


if __name__ == "__main__":
    unittest.main()
