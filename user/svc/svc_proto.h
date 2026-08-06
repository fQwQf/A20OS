/*
 * svc protocol shared between svcman (supervisor) and its services.
 *
 * Design reference: docs/hybrid-kernel/00-design.md §5 (stability).
 *
 * The service endpoint is installed at a fixed slot in the child's handle
 * table via task_spawn's target_slot mechanism (>= A20_NATIVE_FD_HANDLE_BASE
 * installs at exactly that slot), so the service binary can name its
 * service channel with a compile-time constant.
 */
#ifndef _A20_SVC_PROTO_H
#define _A20_SVC_PROTO_H

#include "a20_types.h"
#include "a20_services_idl.h"

#define A20_SVC_ENDPOINT_SLOT   (A20_NATIVE_FD_HANDLE_BASE + 40u)
#define A20_SVC_ENDPOINT_HANDLE ((a20_handle_t)A20_SVC_ENDPOINT_SLOT)

/* Request protocol on the service channel:
 *  - payload "crash" (5 bytes): service exits with code 42 (self-heal demo)
 *  - anything else:             service echoes the payload back
 */
#endif
