"""Interrupt migration between CPU lookup and current-slot load must be safe."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class CurrentMigrationTest(unittest.TestCase):
    def test_indexed_current_with_pending_migration(self):
        source = (ROOT / "kernel/proc/current.c").read_text()
        start = source.index("task_t *proc_current(void)")
        end = source.index("\nvoid proc_io_account", start)
        body = source[start:end]
        harness = r'''
#include <assert.h>
#include <stdint.h>
#define ARCH_FAST_CURRENT 0
#define CONFIG_NR_CPUS 2
typedef struct { int pid; } task_t;
static task_t self = {1}, foreign = {2};
static task_t *g_cpu_current[2] = {&self, &foreign};
static int irqs = 1, migration = 1;
static void migrate(void) {
    migration = 0;
    g_cpu_current[0] = &foreign;
    g_cpu_current[1] = &self;
}
static int arch_irqs_enabled(void) { return irqs; }
static void arch_local_irq_disable(void) { irqs = 0; }
static void arch_local_irq_enable(void) {
    irqs = 1;
    if (migration) migrate();
}
static unsigned cpu_current_id(void) {
    unsigned old_cpu = 0;
    /* Inject at the former unprotected window, after computing the id. */
    if (irqs && migration) migrate();
    return old_cpu;
}
'''
        checks = r'''
int main(void) {
    assert(proc_current() == &self);
    assert(irqs == 1 && migration == 0);
    /* A caller already in an IRQ-disabled region must remain there. */
    irqs = 0; migration = 1;
    g_cpu_current[0] = &self;
    assert(proc_current() == &self);
    assert(irqs == 0 && migration == 1);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as temp:
            src = Path(temp) / "test.c"
            exe = Path(temp) / "test"
            src.write_text(harness + body + checks)
            subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", str(src),
                            "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
            # Negative control: the former indexed expression returns foreign.
            old = body[:body.index("#else\n    /* An IRQ")] + (
                "#else\n    return g_cpu_current[cpu_current_id()];\n"
                "#endif\n}\n")
            # Keep stubs referenced so the negative control has the same flags.
            old_checks = checks.replace("assert(proc_current() == &self);", "assert(proc_current() == &foreign);", 1)
            old_checks = old_checks[:old_checks.index("    /* A caller")] + (
                "    arch_local_irq_disable();\n"
                "    assert(!arch_irqs_enabled());\n"
                "    arch_local_irq_enable();\n    return 0;\n}\n")
            src.write_text(harness + old + old_checks)
            subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", str(src),
                            "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
