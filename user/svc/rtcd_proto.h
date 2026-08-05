/*
 * Slot/protocol conventions for the rtcd user-space RTC driver and its
 * supervisor test (docs/hybrid-kernel/01-roadmap.md phase 4).
 */
#ifndef _A20_RTCD_PROTO_H
#define _A20_RTCD_PROTO_H

#include "a20_types.h"

#define A20_RTCD_EP_SLOT    (A20_NATIVE_FD_HANDLE_BASE + 43u)
#define A20_RTCD_EP_HANDLE  ((a20_handle_t)A20_RTCD_EP_SLOT)

/* QEMU virt goldfish RTC (see hw/rtc/goldfish_rtc.c, linux goldfish driver) */
#define GOLDFISH_RTC_BASE   0x101000ULL
#define GOLDFISH_RTC_SIZE   0x1000ULL
#define GOLDFISH_RTC_IRQ    11u

#define RTC_TIME_LOW        0x00u   /* R: nanoseconds low/high */
#define RTC_TIME_HIGH       0x04u
#define RTC_ALARM_LOW       0x08u   /* RW */
#define RTC_ALARM_HIGH      0x0cu
#define RTC_IRQ_ENABLE      0x10u   /* RW */
#define RTC_CLEAR_ALARM     0x14u   /* W */
#define RTC_ALARM_STATUS    0x18u   /* R */

/* Request protocol on the service channel (text-free binary):
 *   'T'        -> reply { u64 sec, u64 nsec }
 *   'A' + u32  -> arm a one-shot alarm in <ms>; async reply { u64 sec }
 *   'C'        -> exit(42) (crash self-heal demo, mirrors echod)
 */
#define RTCD_REQ_TIME    'T'
#define RTCD_REQ_ALARM   'A'
#define RTCD_REQ_CRASH   'C'

#endif
