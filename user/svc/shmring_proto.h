/*
 * Slot conventions for the shmring benchmark service pair
 * (docs/hybrid-kernel/01-roadmap.md phase 3).
 */
#ifndef _A20_SHMRING_PROTO_H
#define _A20_SHMRING_PROTO_H

#include "a20_types.h"

#define A20_SHMRING_VMO_SLOT   (A20_NATIVE_FD_HANDLE_BASE + 41u)
#define A20_SHMRING_VMO_HANDLE ((a20_handle_t)A20_SHMRING_VMO_SLOT)

#define A20_CHAND_EP_SLOT      (A20_NATIVE_FD_HANDLE_BASE + 42u)
#define A20_CHAND_EP_HANDLE    ((a20_handle_t)A20_CHAND_EP_SLOT)

#define A20_SHMRING_VMO_SIZE   (1024u * 1024u)
#define A20_SHMRING_CAP        (512u * 1024u)
#define A20_SHMRING_TOTAL      (16u * 1024u * 1024u)

#endif
