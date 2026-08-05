/*
 * netd — userspace network daemon (hybrid-kernel M-series).
 *
 * Runs lwIP in user mode (NO_SYS, single-threaded service loop) and
 * owns the network protocol stack; the kernel keeps the virtio-net
 * driver and forwards frames over a shared ring.  This file is the
 * service entry: initializes the stack and runs the frame/service
 * loop (frame ring plumbing lands with the kernel frame-plane
 * commit).
 */
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    lwip_init();

    for (;;) {
        /* Service loop: drain RX frames from the kernel ring into
         * netif_input(), run lwIP timers, and pump TX frames back.
         * (Frame plane lands with the kernel netd ring.) */
        __asm__ volatile("wfi");
    }
}
