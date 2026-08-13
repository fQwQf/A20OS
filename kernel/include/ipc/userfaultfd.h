#ifndef _IPC_USERFAULTFD_H
#define _IPC_USERFAULTFD_H

#include "core/types.h"
#include "core/lock.h"
#include "core/refcount.h"
#include "core/sync.h"

struct mm_struct;
struct task_t;

/*
 * A20OS userfaultfd — user-space page-fault handling for anonymous private
 * ranges.
 *
 * The fd supports the Linux ioctl subset that userland actually uses for the
 * "missing page" protocol:
 *   - UFFDIO_API (negotiate version/features/ioctls)
 *   - UFFDIO_REGISTER  (MISSING mode only, anonymous private ranges)
 *   - UFFDIO_UNREGISTER
 *   - UFFDIO_COPY      (resolve a missing page from handler memory)
 *   - UFFDIO_ZEROPAGE  (resolve a missing page as a zero page)
 *   - UFFDIO_WAKE      (kick waiters; COPY/ZEROPAGE with DONTWAKE)
 * read(2)/poll(2) on the fd deliver UFFD_EVENT_PAGEFAULT messages.
 *
 * The faulting thread parks (PROC_WAIT_KILLABLE) until the handler resolves
 * the page with UFFDIO_COPY/UFFDIO_ZEROPAGE or the range is unregistered.
 * UFFD_FEATURE_EVENT_FORK is not advertised: fork() creates a fresh address
 * space with no userfaultfd ranges, matching a child that simply never
 * registered.
 */

#define UFFD_API_VERSION 0xAAUL
#define UFFD_API_FEATURES 0ULL

/* Events (wire values, matches struct uffd_msg::event). */
#define UFFD_EVENT_PAGEFAULT 0x12
#define UFFD_EVENT_FORK      0x13
#define UFFD_EVENT_REMAP     0x14
#define UFFD_EVENT_REMOVE    0x15
#define UFFD_EVENT_UNMAP     0x16

#define UFFD_PAGEFAULT_FLAG_WRITE (1ULL << 0)
#define UFFD_PAGEFAULT_FLAG_WP    (1ULL << 1)

#define UFFDIO_REGISTER_MODE_MISSING (1ULL << 0)
#define UFFDIO_REGISTER_MODE_WP      (1ULL << 1)

#define UFFDIO_COPY_MODE_DONTWAKE    (1ULL << 0)
#define UFFDIO_ZEROPAGE_MODE_DONTWAKE (1ULL << 0)

/* ioctl command numbers (Linux asm-generic wire format, type 0xAA). */
#define UFFDIO_API         0x4017aa3fUL
#define UFFDIO_REGISTER    0xc01faa00UL
#define UFFDIO_UNREGISTER  0x400faa01UL
#define UFFDIO_WAKE        0x400faa02UL
#define UFFDIO_COPY        0xc027aa03UL
#define UFFDIO_ZEROPAGE    0xc01faa04UL

/* Wire layouts (Linux ABI). */
typedef struct uffdio_range {
    uint64_t start;
    uint64_t len;
} uffdio_range_t;

typedef struct uffdio_api {
    uint64_t api;
    uint64_t features;
    uint64_t ioctls;
} uffdio_api_t;

typedef struct uffdio_register {
    uffdio_range_t range;
    uint64_t mode;
    uint64_t ioctls;
} uffdio_register_t;

typedef struct uffdio_copy {
    uint64_t dst;
    uint64_t src;
    uint64_t len;
    uint64_t mode;
    int64_t  copy;
} uffdio_copy_t;

typedef struct uffdio_zeropage {
    uffdio_range_t range;
    uint64_t mode;
    int64_t  zeropage;
} uffdio_zeropage_t;

typedef struct uffd_msg {
    uint8_t  event;
    uint8_t  reserved1;
    uint16_t reserved2;
    uint32_t reserved3;
    union {
        struct {
            uint64_t flags;
            uint64_t address;
            uint32_t ptid;
            uint32_t reserved;
        } pagefault;
        struct {
            uint32_t ufd;
        } fork;
        struct {
            uint64_t from;
            uint64_t to;
            uint64_t len;
        } remap;
        struct {
            uint64_t start;
            uint64_t end;
        } remove;
        struct {
            uint64_t reserved1;
            uint64_t reserved2;
            uint64_t reserved3;
        } reserved;
    } arg;
} __attribute__((packed)) uffd_msg_t;

/* Kernel-side userfaultfd object (opaque to callers). */
typedef struct userfaultfd userfaultfd_t;

/* fd creation entry point used by the Linux ABI syscall layer. */
int userfaultfd_create_file(unsigned flags);

/*
 * Fast-path predicate for the demand-fault handler: returns 1 when a
 * MISSING-mode range of @mm covers @page_va, so the fault path hands the
 * fault to userfaultfd_handle_fault() instead of materializing a zero page.
 */
int userfaultfd_range_present(struct mm_struct *mm, uint64_t page_va);

/*
 * Fault-path hook.  Called from the demand-fault handler for an anonymous
 * private page when a MISSING-mode range of @mm covers @page_va.  Enqueues
 * a PAGEFAULT event, wakes readers, then parks the current task until the
 * handler resolves the page or the range is unregistered.  Returns 0 when
 * the caller should retry the fault (page present or range unregistered),
 * or -1 on fatal-signal interruption.
 */
int userfaultfd_handle_fault(struct task_t *t, struct mm_struct *mm,
                             uint64_t page_va, int write_access);

/*
 * Release every registered range that references @mm.  Called from mm_destroy()
 * when the final reference to an address space is dropped.
 */
void userfaultfd_mm_cleanup(struct mm_struct *mm);

#endif /* _IPC_USERFAULTFD_H */
