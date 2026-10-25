"""Exercise the production x86 UART burst policy against a bounded fake FIFO."""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class UartTransmitTests(unittest.TestCase):
    def test_fifo_limits_order_and_legacy_fallback(self):
        source = (ROOT / 'kernel/arch/x86_64/include/console.h').read_text()
        start = source.index('static inline void arch_uart_write(')
        body = source[start:source.index('\nstatic inline void arch_uart_putc', start)]
        asm = '''__asm__ __volatile__("rep outsb" : "+S"(data), "+c"(n)
                             : "d"((uint16_t)0x3F8) : "memory");'''
        self.assertIn(asm, body)
        body = body.replace(asm, 'fake_outsb(&data, n);')
        fixture = r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <string.h>
static char output[5000];
static size_t used, fifo_depth, polls, bursts, waiting;
static uint8_t inb(uint16_t port) {
    if (port == 0x3fa) return fifo_depth == 16 ? 0xc0 : 0;
    assert(port == 0x3fd); polls++;
    if (waiting) { waiting--; return 0; }
    return 0x20;
}
static void fake_outsb(const char **data, size_t n) {
    assert(n && n <= fifo_depth && used + n <= sizeof(output));
    memcpy(output + used, *data, n); *data += n; used += n; bursts++;
    waiting = 2; /* The FIFO is busy before each following burst. */
}
'''
        fixture += body + r'''
int main(void) {
    char input[4097];for(size_t i=0;i<sizeof(input);i++)input[i]=(char)(i*37);
    size_t lengths[]={1,2,15,16,17,31,32,255,256,257,4096,4097};
    for(size_t depth=1;depth<=16;depth+=15)for(unsigned j=0;j<sizeof(lengths)/sizeof(*lengths);j++) {
        used=bursts=polls=waiting=0;fifo_depth=depth;
        arch_uart_write(input,lengths[j]);
        assert(used==lengths[j]&&!memcmp(input,output,used));
        assert(bursts==(used+depth-1)/depth);
        assert(polls==bursts+2*(bursts-1));
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            test = Path(tmp) / 'uart.c'
            test.write_text(fixture)
            exe = Path(tmp) / 'uart'
            subprocess.run(['cc', '-std=gnu99', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', str(test), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
