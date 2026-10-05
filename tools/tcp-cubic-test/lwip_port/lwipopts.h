/*
 * Host-test lwipopts.h for tools/test-tcp-cubic-host.c.
 *
 * NOT the kernel's: kernel/net/lwip_port/ ships freestanding <stdio.h>,
 * <stdlib.h> and <string.h> shims that shadow the host libc ones, so pulling it
 * in here would compile the test against the kernel's own printf.  This file
 * declares only what tcp_cubic_priv.h and tcp_cubic.c actually consult, which
 * is LWIP_TCP_CUBIC -- so the header under test is the real one and the
 * algorithm is compiled exactly as the kernel compiles it.
 *
 * Anything the algorithm starts depending on must be added here, and the test
 * will fail to compile until it is -- which is the intended behaviour: the
 * test cannot silently drift from the shipped configuration.
 */
#ifndef A20_TCP_CUBIC_TEST_LWIPOPTS_H
#define A20_TCP_CUBIC_TEST_LWIPOPTS_H

#define LWIP_TCP_CUBIC 1

/* The algorithm's translation unit is guarded by these; the test does not
 * exercise IP, but tcp.h pulls in the address types. */
#define LWIP_IPV4 1
#define LWIP_IPV6 1
#define LWIP_TCP 1
#define NO_SYS 1
#define SYS_LIGHTWEIGHT_PROT 1

/* Pin the window geometry to the DEFAULT/SERVER tier of
 * kernel/net/net_profile.h:124-125, which is what kernel/net/lwip_port/lwipopts.h
 * hands to lwIP in every non-EMBEDDED build.  This is not cosmetic.  lwIP picks
 * tcpwnd_size_t from LWIP_WND_SCALE, not from the value of TCP_WND
 * (lwip/tcpbase.h:49-54), and the kernel turns that on (lwipopts.h:121) so its
 * windows are u32_t.  Leave it at opt.h's stock 0 and the type is silently u16_t
 * here while the kernel runs u32_t: the test would then exercise 16-bit window
 * arithmetic the kernel never performs, and every comparison in the algorithm
 * would pass while the shipped code overflowed.  The test then reported exactly
 * that -- a 140160 B window coming back as 9088 -- which is how the drift was
 * caught at all. */
#define TCP_MSS        1460
#define TCP_WND        (64 * TCP_MSS)
#define TCP_SND_BUF    (64 * TCP_MSS)
#define LWIP_WND_SCALE 1
#define TCP_RCV_SCALE  3

#endif
