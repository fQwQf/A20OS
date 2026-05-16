/*
 * A20OS Native ABI — Kernel object types for handle-backed objects.
 * Design references:
 *   docs/native-abi/05-ipc.md §2–3 — channel, eventq
 *   docs/native-abi/03-handle.md §1.1 — object type mapping
 *   docs/native-abi/04-memory.md §4.5 — shm
 *   docs/native-abi/06-security.md §7–8 — namespace, debug
 */
#ifndef _ABI_NATIVE_OBJECTS_H
#define _ABI_NATIVE_OBJECTS_H

#include "core/types.h"
#include "core/lock.h"
#include "core/refcount.h"
#include "abi/native/vmo.h"

struct a20_socket {
    refcount_t  refcount;
    int         kern_fd;
};

struct a20_shm {
    refcount_t  refcount;
    struct a20_vmo *vmo;
    uint32_t    export_rights;
};

struct a20_namespace {
    refcount_t  refcount;
    uint32_t    ns_type;
    uint32_t    flags;
    void       *isolated_data;
    char        root_path[256];
    uint32_t    net_ifindex;
    uint64_t    pid_offset;
    uint32_t    dev_access_mask;
};

struct a20_debug {
    refcount_t  refcount;
    struct task_struct *target;
    uint32_t    options;
};

#endif
