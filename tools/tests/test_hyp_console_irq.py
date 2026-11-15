"""The virtual UART has no IRQ; an ordinary QEMU UART retains IRQ 10."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class HypConsoleIrq(unittest.TestCase):
    def test_guest_flag_is_a_whole_token_and_available_before_bootargs_init(self):
        source = (ROOT / 'kernel/arch/riscv64/platform/console.c').read_text()
        source = '\n'.join(line for line in source.splitlines()
                           if not line.startswith('#include'))
        harness = r'''
#include <assert.h>
#include <string.h>
#define UART0_IRQ 10
#define CONFIG_BOARD_QEMU_VIRT_RISCV64 1
static const char *args;
static const char *arch_bootargs_get(void) { return args; }
''' + source + r'''
int main(void) {
    args = 0; assert(arch_uart_irq() == 10);
    args = ""; assert(arch_uart_irq() == 10);
    args = "a20.hypguest=1"; assert(arch_uart_irq() == -1);
    args = "other=1\ta20.hypguest=1 "; assert(arch_uart_irq() == -1);
    args = "a20.hypguest=0"; assert(arch_uart_irq() == 10);
    args = "a20.hypguest=10"; assert(arch_uart_irq() == 10);
    args = "xa20.hypguest=1"; assert(arch_uart_irq() == 10);
}
'''
        with tempfile.TemporaryDirectory() as d:
            path = Path(d)
            (path / 'main.c').write_text(harness)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            str(path / 'main.c'), '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()
