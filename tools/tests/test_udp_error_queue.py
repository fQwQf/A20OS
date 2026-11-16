"""Exercise production ICMP parsing and bounded extended-error ownership."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
PRE = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "net/icmp_error.h"
#define AF_INET 2
#define AF_INET6 10
#define NET_MAX_PAYLOAD 65507
#define PROC_WAKE_EVENT 1
/* ERROR_TYPES */
typedef struct net_socket {
 int domain, ipv4_recv_err, ipv6_recv_err, closed, error_count, pending_error;
 uint32_t rcv_buf;
 size_t error_bytes;
 net_error_msg_t *error_head, *error_tail;
 int read_waitq;
} net_socket_t;
static int locked, allocations, fail_next, wakes;
static void *kmalloc(size_t n) {
 assert(!locked);
 if (fail_next) {fail_next=0;return NULL;}
 void *p=malloc(n);if(p)allocations++;return p;
}
static void kfree(void *p) {assert(!locked);if(p){allocations--;free(p);}}
static uint64_t net_sock_lock(net_socket_t *s){(void)s;assert(!locked);locked=1;return 0;}
static void net_sock_unlock(net_socket_t *s,uint64_t f){(void)s;(void)f;assert(locked);locked=0;}
static void wait_queue_wake_all(int *q,int v,int reason){(void)q;(void)v;(void)reason;assert(!locked);wakes++;}
'''
POST = r'''
int main(void) {
 net_socket_t s={.domain=2,.ipv4_recv_err=1,.rcv_buf=65536};
 net_extended_error_t e={.error=111,.origin=2,.type=3,.code=3};
 uint8_t address[28]={2}, offender[28]={2};
 fail_next=1;
 assert(net_error_enqueue(&s,&e,address,offender,16,"abc",3)==-ENOMEM);
 assert(!s.error_count&&!allocations&&!wakes);
 for(int i=0;i<8;i++){char p=(char)i;assert(net_error_enqueue(&s,&e,address,offender,16,&p,1)==0);}
 assert(s.error_count==8&&allocations==8&&wakes==8&&s.pending_error==111);
 assert(net_error_enqueue(&s,&e,address,offender,16,"x",1)==-ENOBUFS);
 assert(allocations==8);
 for(int i=0;i<8;i++) {
  net_error_msg_t *m=net_error_dequeue(&s);
  assert(m&&m->payload[0]==i&&m->error.error==111&&m->address_len==16);
  kfree(m);assert(s.pending_error==(i==7?0:111));
 }
 assert(!net_error_dequeue(&s)&&!s.error_bytes&&!allocations);
 s.rcv_buf=sizeof(net_error_msg_t)-1;
 assert(net_error_enqueue(&s,&e,address,offender,16,NULL,0)==-ENOBUFS);
 s.rcv_buf=65536;s.ipv4_recv_err=0;
 assert(net_error_enqueue(&s,&e,address,offender,16,NULL,0)==-ENOBUFS);
 s.domain=10;s.ipv6_recv_err=1;e.origin=3;e.type=1;e.code=4;
 assert(net_error_enqueue(&s,&e,address,offender,28,NULL,0)==0);
 net_error_purge(&s);assert(!allocations&&!s.error_count&&!s.error_bytes);
 uint8_t p[100]={0};net_icmp_error_t parsed;
 p[0]=0x45;p[9]=1;p[20]=3;p[21]=3;p[28]=0x45;p[37]=17;
 p[40]=127;p[43]=1;p[44]=127;p[47]=1;
 p[48]=4;p[49]=210;p[51]=53;p[53]=12;memcpy(p+56,"data",4);
 assert(net_icmp_error_parse(2,p,60,&parsed));
 assert(parsed.error==111&&parsed.origin==2&&parsed.payload_offset==56&&parsed.payload_len==4);
 assert(parsed.source_port[0]==4&&parsed.source_port[1]==210);
 for(size_t n=0;n<60;n++) (void)net_icmp_error_parse(2,p,n,&parsed);
 p[35]=1;assert(!net_icmp_error_parse(2,p,60,&parsed));p[35]=0;
 p[21]=4;p[26]=5;p[27]=220;
 assert(net_icmp_error_parse(2,p,60,&parsed)&&parsed.error==90&&parsed.info==1500);
 memset(p,0,sizeof(p));p[0]=1;p[1]=4;p[8]=0x60;p[14]=17;
 p[23]=1;p[39]=1;p[49]=7;p[51]=53;p[53]=12;memcpy(p+56,"data",4);
 assert(net_icmp_error_parse(10,p,60,&parsed));
 assert(parsed.origin==3&&parsed.error==111&&parsed.payload_len==4);
 for(size_t n=0;n<60;n++) (void)net_icmp_error_parse(10,p,n,&parsed);
 p[0]=2;p[1]=0;p[6]=5;
 assert(net_icmp_error_parse(10,p,60,&parsed)&&parsed.info==1280&&parsed.error==90);
 return 0;
}
'''


class UdpErrorQueue(unittest.TestCase):
    def test_parser_and_queue_ownership(self):
        header = (ROOT / 'kernel/net/socket/socket_internal.h').read_text()
        types = header[header.index('typedef struct net_extended_error'):header.index('typedef struct net_socket {')]
        queue = (ROOT / 'kernel/net/socket/socket_queue.c').read_text()
        queue = queue[queue.index('/* Error records are bounded'):]
        with tempfile.TemporaryDirectory(prefix='a20-udp-errors-') as directory:
            source = Path(directory) / 'error_queue.c'
            binary = source.with_suffix('')
            source.write_text(PRE.replace('/* ERROR_TYPES */', types) + queue + POST)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I'+str(ROOT/'kernel/include'),
                            str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == '__main__':
    unittest.main()
