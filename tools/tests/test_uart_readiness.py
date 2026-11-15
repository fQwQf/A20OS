"""A poll-before-read client must see FIFO data without another UART IRQ."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class UartReadiness(unittest.TestCase):
    def test_polled_console_selects_timed_readiness_fallback(self):
        uart = (ROOT / "kernel/drivers/char/uart.c").read_text()
        begin = uart.index("wait_queue_t *uart_read_wait_queue(void)")
        queue = uart[begin:uart.index("\nvoid uart_puts", begin)]
        devfs = (ROOT / "kernel/fs/devfs/devfs.c").read_text()
        begin = devfs.index("static size_t devfs_tty_poll_sources(")
        poll = devfs[begin:devfs.index("\nstatic vfile_ops_t g_devfs_tty_ops", begin)]
        fixture = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#define POLLIN 1
typedef struct { int unused; } wait_queue_t;
typedef struct { int unused; } vfile_t;
typedef struct { wait_queue_t *queue; uintptr_t key; uint64_t deadline; } readiness_source_t;
static struct { int uart_rx_is_polled; } board;
static typeof(board) *current_board;
static wait_queue_t rx_waiters;
static int irq = 10;
static int arch_uart_irq(void) { return irq; }
''' + queue + poll + r'''
int main(void) {
    readiness_source_t source = {0};
    current_board = &board;
    board.uart_rx_is_polled = 0;
    assert(devfs_tty_poll_sources(NULL, POLLIN, &source, 1) == 1);
    assert(source.queue == &rx_waiters);
    irq = -1;
    assert(devfs_tty_poll_sources(NULL, POLLIN, &source, 1) == 0);
    irq = 10;
    board.uart_rx_is_polled = 1;
    /* A zero source count activates readiness_wait_once's existing deadline. */
    assert(devfs_tty_poll_sources(NULL, POLLIN, &source, 1) == 0);
    assert(devfs_tty_poll_sources(NULL, 0, &source, 1) == 0);
    assert(devfs_tty_poll_sources(NULL, POLLIN, &source, 0) == 0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "polled.c"
            binary = path.with_suffix("")
            path.write_text(fixture)
            subprocess.run(["cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror",
                            str(path), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_fifo_tail_byte_order_and_signal_locking(self):
        source = (ROOT / "kernel/drivers/char/uart.c").read_text()
        begin = source.index("int uart_has_input(void)")
        body = source[begin:source.index("\nwait_queue_t *uart_read_wait_queue", begin)]
        fixture = r'''
#include <assert.h>
#include <stdint.h>
typedef struct { int unused; } proc_wake_q_t;
static unsigned rx_head, rx_tail;
static unsigned long rx_polls_rx_bytes;
static int rx_lock, held, hardware = -1, polls, delivered, pushed;
static char queued;
static void proc_wake_q_init(proc_wake_q_t *q) { (void)q; }
static uint64_t spin_lock_irqsave(int *p) { (void)p; assert(!held); held=1; return 0; }
static void spin_unlock_irqrestore(int *p, uint64_t f) { (void)p;(void)f;assert(held);held=0; }
static int arch_uart_poll_getc(void) { assert(held); polls++; int c=hardware;hardware=-1;return c; }
static int uart_rx_push_locked(char c, proc_wake_q_t *q) {
    (void)q;assert(held);pushed++;
    if(c==3)return 2;
    queued=c;rx_head++;return 0;
}
static int proc_wake_q_flush(proc_wake_q_t *q) { (void)q;assert(!held);return 0; }
static void tty_console_isig_raise(int sig) { assert(!held&&sig==2);delivered++; }
''' + body + r'''
int main(void) {
    /* IRQ ring empty, hardware has fewer bytes than its RX threshold. */
    hardware='a';assert(uart_has_input()==1&&queued=='a'&&rx_polls_rx_bytes==1);
    /* Do not consume a later hardware byte ahead of queued input. */
    hardware='b';int old=polls;assert(uart_has_input()==1&&polls==old&&hardware=='b');
    rx_tail=rx_head;assert(uart_has_input()==1&&queued=='b');
    rx_tail=rx_head;assert(uart_has_input()==0);
    hardware=3;assert(uart_has_input()==0&&delivered==1&&pushed==3);
    assert(!held);return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "readiness.c"
            binary = path.with_suffix("")
            path.write_text(fixture)
            subprocess.run(["cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", str(path), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
