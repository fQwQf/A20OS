"""Exercise the real external TCP send loop with allocation/output failures."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class TcpSendBackpressure(unittest.TestCase):
    def test_accepted_bytes_and_retry(self):
        source = (ROOT / 'kernel/net/socket_inet.c').read_text()
        start = source.index('    size_t sent = 0;', source.index('static int net_inet_send_tcp('))
        end = source.index('\nint net_inet_sendto(', start)
        loop = source[start:end]
        harness = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define ERESTARTSYS 512
#define ERR_OK 0
#define ERR_MEM -1
#define ERR_CONN -11
#define ERR_CLSD -15
#define TCP_SND_BUF 65535
#define TCP_SND_QUEUELEN 16
#define TCP_WRITE_FLAG_COPY 1
#define PROC_WAIT_INTERRUPTIBLE 1
#define PROC_WAKE_EVENT 1
#define PROC_WAKE_CANCEL 2
#define PROC_WAKE_TIMEOUT 3
typedef int err_t;
typedef uint16_t u16_t;
typedef unsigned tcpwnd_size_t;
typedef int task_t;
typedef int net_inet_ip_options_snapshot_t;
typedef int proc_wake_reason_t;
typedef int wait_queue_entry_t;
typedef struct { task_t *task; unsigned seq; } proc_wait_token_t;
struct tcp_pcb { unsigned snd_buf, snd_queuelen; };
typedef struct { struct tcp_pcb *tcp; int closed, connected, nonblock, write_waitq; } net_socket_t;
static unsigned ticks, accepted, attempts, output_calls, parks;
static int mode, locked;
static task_t task;
static uint64_t timer_get_ticks(void) { return ticks; }
static uint64_t net_inet_socket_ip_lane_lock(net_socket_t *s, void *opts) {
    (void)s; (void)opts; assert(!locked); locked=1; return 0;
}
static uint64_t net_inet_socket_lane_lock(net_socket_t *s) {
    return net_inet_socket_ip_lane_lock(s, NULL);
}
static void a20_lwip_lane_unlock(uint64_t flags) { (void)flags; assert(locked); locked=0; }
static void a20_lwip_poll_locked(void) { assert(locked); }
static void net_inet_ip_opts_apply_snapshot_locked(net_socket_t *s, void *opts) { (void)s; (void)opts; }
static err_t tcp_write(struct tcp_pcb *pcb, const void *buf, u16_t len, int flags) {
    (void)pcb; (void)buf; (void)flags; assert(locked); attempts++;
    if (mode == 1 && len > 2) return ERR_MEM;
    if (mode == 2 || (mode == 3 && !parks)) return ERR_MEM;
    accepted += len; return ERR_OK;
}
static err_t tcp_output(struct tcp_pcb *pcb) {
    (void)pcb; assert(locked); output_calls++; return -2;
}
#define ktrace_net(...) ((void)0)
static task_t *proc_current(void) { return &task; }
static int net_task_has_unblocked_signal(task_t *t) { (void)t; return 0; }
static int net_socket_wait_expired(net_socket_t *s, uint64_t start, int send) {
    (void)s; (void)start; (void)send; return 0;
}
static uint64_t net_socket_timeout_snapshot(net_socket_t *s, int send) { (void)s; (void)send; return 0; }
static proc_wait_token_t proc_park_prepare(int flags, uint64_t deadline) {
    (void)flags; assert(deadline == ticks+1); return (proc_wait_token_t){&task, 1};
}
static uint64_t net_sock_lock(net_socket_t *s) { (void)s; return 0; }
static void net_sock_unlock(net_socket_t *s, uint64_t flags) { (void)s; (void)flags; }
static bool wait_queue_link(int *q, int *e, proc_wait_token_t t, int flags) {
    (void)q; (void)e; (void)t; (void)flags; return true;
}
static int proc_try_wake(task_t *t, unsigned seq, int why) { (void)t; (void)seq; (void)why; assert(0); return 0; }
static int proc_park_commit(proc_wait_token_t token) { (void)token; parks++; ticks++; return PROC_WAKE_TIMEOUT; }
static int proc_park_cancel(proc_wait_token_t token) { (void)token; return 0; }
static void wait_queue_unlink(int *q, int *e) { (void)q; (void)e; }
static void proc_park_finish(proc_wait_token_t token) { (void)token; }
static int proc_wake_reason_is_task_interrupt(int reason) { (void)reason; return 0; }
static void net_inet_bottom_half_process_all(void) { assert(!locked); }
static void net_packet_bottom_half_process(void) {}
static int send_loop(net_socket_t *s, const void *buf, size_t len) {
'''
        check = r'''
int main(void) {
    struct tcp_pcb pcb={65535,0};
    net_socket_t s={.tcp=&pcb,.connected=1,.nonblock=1};
    mode=1;
    assert(send_loop(&s, "abcdefgh", 8)==8);
    assert(accepted==8 && output_calls>1 && !locked);
    accepted=output_calls=attempts=0; mode=2;
    assert(send_loop(&s, "abcdefgh", 8)==-EAGAIN);
    assert(!accepted && !output_calls && !locked);
    s.nonblock=0; mode=3;
    assert(send_loop(&s, "abcdefgh", 8)==8);
    assert(parks==1 && accepted==8 && !locked);
    s.nonblock=1; pcb.snd_queuelen=TCP_SND_QUEUELEN;
    assert(send_loop(&s, "a", 1)==-EAGAIN);
    s.closed=1;
    assert(send_loop(&s, "a", 1)==-EPIPE);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / 'tcp.c', Path(tmp) / 'tcp'
            src.write_text(harness + loop + check)
            subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
