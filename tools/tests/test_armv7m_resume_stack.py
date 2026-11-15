"""Exercise the production PSP admission guard, including invalid stacks."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ResumeStackTests(unittest.TestCase):
    def test_only_owned_privileged_psp_syscall_stacks_are_admitted(self):
        source = (ROOT / 'kernel/arch/armv7m/platform/program.c').read_text()
        # Compile the actual production functions with minimal data-layout
        # shims; no emulated scheduler or independent copy of the predicate.
        functions = []
        for name in ('within', 'armv7m_program_resume_stack_valid'):
            match = re.search(r'^(?:static )?int ' + name + r'\([^\n]*\) \{.*?^\}',
                              source, re.M | re.S)
            self.assertIsNotNone(match)
            functions.append(match.group())
        harness = r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
typedef struct { unsigned control; } task_context_t;
typedef struct {
    void *owner, *stack;
    size_t stack_size;
    unsigned in_kernel;
} armv7m_program_t;
typedef struct {
    armv7m_program_t *arch_program;
    uintptr_t kstack;
} task_t;
''' + '\n'.join(functions) + r'''
int main(void) {
    task_context_t ctx = {.control = 2};
    armv7m_program_t p = {.owner = &ctx, .stack = (void *)0x60001000,
                         .stack_size = 4096, .in_kernel = 1};
    task_t t = {.arch_program = &p, .kstack = (uintptr_t)&ctx};
    assert(armv7m_program_resume_stack_valid(&t, 0x60001000));
    assert(armv7m_program_resume_stack_valid(&t, 0x60002000));
    assert(!armv7m_program_resume_stack_valid(&t, 0x60000fff));
    assert(!armv7m_program_resume_stack_valid(&t, 0x60002001));
    assert(!armv7m_program_resume_stack_valid(&t, UINTPTR_MAX));
    assert(!armv7m_program_resume_stack_valid(NULL, 0x60001800));
    for (unsigned control = 0; control < 4; control++) {
        ctx.control = control;
        assert(!!armv7m_program_resume_stack_valid(&t, 0x60001800) == (control == 2));
    }
    ctx.control = 2;
    p.in_kernel = 0;
    assert(!armv7m_program_resume_stack_valid(&t, 0x60001800));
    p.in_kernel = 1;
    p.owner = NULL;
    assert(!armv7m_program_resume_stack_valid(&t, 0x60001800));
    p.owner = &ctx;
    p.stack = NULL;
    assert(!armv7m_program_resume_stack_valid(&t, 0));
    t.arch_program = NULL;
    assert(!armv7m_program_resume_stack_valid(&t, 0x60001800));
    t.arch_program = &p;
    t.kstack = 0;
    assert(!armv7m_program_resume_stack_valid(&t, 0x60001800));
}
'''
        with tempfile.TemporaryDirectory(prefix='a20-mcu-stack-') as directory:
            path = Path(directory)
            (path / 'guard.c').write_text(harness)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=undefined', str(path / 'guard.c'),
                            '-o', str(path / 'guard')], check=True)
            subprocess.run([str(path / 'guard')], check=True)


if __name__ == '__main__':
    unittest.main()
