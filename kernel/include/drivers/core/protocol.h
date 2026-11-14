/* SPDX-License-Identifier: Apache-2.0 */
#ifndef A20_DRIVER_PROTOCOL_H
#define A20_DRIVER_PROTOCOL_H

#include "core/types.h"

struct device;
/* One binding publication, shared with legacy class adapters. It is not a
 * device reference or a user capability, and never follows a later rebind. */
typedef struct class_device driver_endpoint_t;

#define DRIVER_PROTOCOL_CHAR    0x4132300000000001ULL
#define DRIVER_PROTOCOL_BLOCK   0x4132300000000002ULL
#define DRIVER_PROTOCOL_NET     0x4132300000000003ULL
#define DRIVER_PROTOCOL_INPUT   0x4132300000000004ULL
#define DRIVER_PROTOCOL_DISPLAY 0x4132300000000005ULL
#define DRIVER_PROTOCOL_AUDIO   0x4132300000000006ULL
#define DRIVER_PROTOCOL_NET_STATS 0x4132300000010001ULL

struct driver_protocol_info {
    uint64_t id;
    uint32_t version;       /* exact semantic version; append-only prefix growth */
    uint32_t ops_size;      /* actual provider table size, never inferred */
    uint64_t capabilities; /* negotiated features, not access rights */
};
struct driver_protocol_descriptor {
    struct driver_protocol_info info;
    const void *ops;
};

/* Current probe only, before publication. Copies the descriptor into this
 * binding's existing devres; table/code must outlive binding teardown.
 * Duplicate IDs (including the automatic primary class protocol) -> EEXIST.
 * Invalid descriptor/context -> EINVAL, metadata OOM -> ENOMEM. No hardware
 * access, publication or ownership transfer on failure. Tables are immutable
 * after registration. This does not grant DMA or user-space authority. */
int device_add_protocol(struct device *, const struct driver_protocol_descriptor *);

/* Caller holds a device reference. Returns an owned publication reference,
 * possibly concurrently offline. No lifecycle mutex or hardware access. */
driver_endpoint_t *device_endpoint_get(const struct device *);
void driver_endpoint_get(driver_endpoint_t *);
void driver_endpoint_put(driver_endpoint_t *);
/* Caller holds endpoint. Metadata is immutable and readable after offline.
 * Enumeration is sorted by ID; info never exposes a module operation table. */
size_t driver_endpoint_protocol_count(const driver_endpoint_t *);
int driver_endpoint_protocol_info(const driver_endpoint_t *, size_t index,
                                    struct driver_protocol_info *out);

struct driver_protocol_requirement {
    uint64_t id;
    uint32_t version;
    uint32_t ops_size; /* required prefix, including every accessed member */
    uint64_t capabilities;
};
struct driver_protocol_access {
    driver_endpoint_t *endpoint;
    struct device *device;
    const void *ops;
    struct driver_protocol_info info;
};
/* Caller owns endpoint throughout begin/access/end. Success borrows that
 * reference and owns one active call, protecting the exact binding/table/
 * instance until end. Admission/end do not allocate or run finalizers and
 * are IRQ-safe; individual protocol methods define their own context rules.
 * Unsupported ID/prefix/features -> EOPNOTSUPP, version -> EPROTONOSUPPORT,
 * offline -> ENODEV. Errors clear out. Never wait for lifecycle operations
 * which need this call to drain.
 * Do not copy an access or begin over a live one. Async requests must retain
 * their own resources; this scope does not keep returned storage alive. */
int driver_protocol_begin(driver_endpoint_t *, const struct driver_protocol_requirement *,
                             struct driver_protocol_access *out);
/* Clear before releasing the active call. Does not put the borrowed endpoint.
 * NULL/empty is harmless. */
void driver_protocol_end(struct driver_protocol_access *);

#endif
