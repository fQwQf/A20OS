/*
 * Slot/protocol conventions for the rtcd user-space RTC driver and its
 * supervisor test (docs/hybrid-kernel/01-roadmap.md phase 4).
 *
 * The goldfish RTC register map and MMIO base/IRQ live in the shared
 * dual-placement header kernel/include/drivers/dual/goldfish_rtc.h;
 * this header keeps only the service protocol.
 */
#ifndef _A20_RTCD_PROTO_H
#define _A20_RTCD_PROTO_H

#include "a20_types.h"
#include "a20_services_idl.h"

#define A20_RTCD_EP_SLOT    (A20_NATIVE_FD_HANDLE_BASE + 43u)
#define A20_RTCD_EP_HANDLE  ((a20_handle_t)A20_RTCD_EP_SLOT)

/* QEMU virt goldfish RTC identity (duplicated from the shared header so
 * protocol-only consumers need no kernel include path). */
#define GOLDFISH_RTC_BASE   0x101000ULL
#define GOLDFISH_RTC_SIZE   0x1000ULL
#define GOLDFISH_RTC_IRQ    11u

/* Request protocol on the service channel (text-free binary):
 *   'T'        -> reply { u64 sec, u64 nsec }
 *   'A' + u32  -> arm a one-shot alarm in <ms>; async reply { u64 sec }
 *   'C'        -> exit(42) (crash self-heal demo, mirrors echod)
 */
#endif
