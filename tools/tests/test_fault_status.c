/*
 * Host differential harness for kernel/mm/fault_status.rs.
 *
 * The production Rust is linked against a scripted model of the status-led
 * demand fault: a cursor whose query facts the scenario chooses, and counters
 * for every primitive.  The assertions are about the decisions Rust owns -- the
 * decline conditions and that each one closes the cursor, the prot round-trip
 * and the access check that follows it, the allocate/charge/zero/map order with
 * its rollback, and the accounting that runs after the cursor is closed.
 *
 * Guest coverage is separate: the smoke cases assert fault_from_status
 * non-vacuity, so the migrated path is exercised rather than bypassed.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PFN_NONE 0xFFFFFFFFu
#define MM_ST_ANON_VIRT 1u
#define MM_ST_ANON_MAPPED 2u
#define MM_ST_PROT_R 0x01u
#define MM_ST_PROT_W 0x02u
#define MM_ST_PROT_X 0x04u
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MM_FAULT_ACCESS_READ 0
#define MM_FAULT_ACCESS_WRITE 1
#define MM_FAULT_ACCESS_EXEC 2

static int failures, checks;

static void check(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

/* ---------------------------------------------------------------- model -- */

static int open_rc, open_cls, open_already, open_uffd;
static int open_calls, close_calls, map_calls, alloc_calls, charge_calls;
static int uncharge_calls, zero_calls, rss_calls, bump_calls, flush_calls;
static uint32_t alloc_rc;
static int charge_rc, map_rc;
static int map_cls, map_va_ok;
static uintptr_t map_pa, map_allow;
static int map_while_open, alloc_while_open;
static int prot_calls, last_prot;
static uintptr_t last_flush_stval;
static int frame_puts, rss_before_bump, close_before_rss;
static int g_open_depth;

static void model_reset(void)
{
    open_rc = 1; open_cls = (int)((MM_ST_ANON_VIRT << 4) | MM_ST_PROT_R | MM_ST_PROT_W);
    open_already = 0; open_uffd = 0;
    open_calls = close_calls = map_calls = alloc_calls = charge_calls = 0;
    uncharge_calls = zero_calls = rss_calls = bump_calls = flush_calls = 0;
    alloc_rc = 0x2000; charge_rc = 0; map_rc = 0;
    map_cls = -1; map_va_ok = 0;
    map_pa = 0; map_allow = 0;
    map_while_open = alloc_while_open = 0;
    prot_calls = 0; last_prot = 0;
    last_flush_stval = 0;
    frame_puts = 0; rss_before_bump = 0; close_before_rss = 0;
    g_open_depth = 0;
}

/* --------------------------------------------------------------- bridges - */

int a20_fault_status_open(void *ctx, void *mm, uintptr_t va, uint8_t *cls_out,
                          int *already_out, int *uffd_out)
{
    (void)ctx; (void)mm; (void)va;
    open_calls++;
    *cls_out = 0; *already_out = 0; *uffd_out = 0;
    if (!open_rc)
        return 0;
    g_open_depth++;
    *cls_out = (uint8_t)open_cls;
    *already_out = open_already;
    *uffd_out = open_uffd;
    return 1;
}

int a20_fault_status_map_if_absent(void *ctx, uintptr_t va, uintptr_t pa,
                                   uintptr_t allow, uint8_t cls)
{
    (void)ctx;
    map_calls++;
    if (g_open_depth != 0) map_while_open++;
    map_pa = pa; map_allow = allow; map_cls = cls;
    map_va_ok = (va == 0x40000);
    return map_rc;
}

void a20_fault_status_close(void *ctx)
{
    (void)ctx;
    close_calls++;
    g_open_depth--;
}

uint32_t a20_fault_status_alloc(void)
{
    alloc_calls++;
    if (g_open_depth != 0) alloc_while_open++;
    return alloc_rc;
}

uintptr_t a20_fault_status_phys(uint32_t pfn) { return (uintptr_t)pfn << 12; }

int a20_fault_status_charge(void *t) { (void)t; charge_calls++; return charge_rc; }

void a20_fault_status_uncharge(void *t) { (void)t; uncharge_calls++; }

void a20_fault_status_zero(uint32_t pfn) { (void)pfn; zero_calls++; }

void a20_fault_status_rss_add(void *mm)
{
    (void)mm;
    rss_calls++;
    if (close_calls == 0) close_before_rss = 1;
}

void a20_fault_status_bump(void *t)
{
    (void)t;
    bump_calls++;
    rss_before_bump = rss_calls;
}

void a20_fault_status_flush_local(uintptr_t stval)
{
    flush_calls++;
    last_flush_stval = stval;
}

void frame_put(uint32_t pfn) { (void)pfn; frame_puts++; }

uintptr_t mm_prot_to_pte_flags(int prot)
{
    prot_calls++;
    last_prot = prot;
    return 0x1000u | (uintptr_t)prot; /* a distinguishable allow value */
}

extern int a20_fault_from_status_rust(void *t, void *mm, uintptr_t page_va,
                                      uintptr_t stval, int access);

/* ------------------------------------------------------------- scenarios - */

static void *const T = (void *)(uintptr_t)0x7B0;
static void *const MM = (void *)(uintptr_t)0x9B0;
#define VA 0x40000u
#define STVAL (0x40000u | 0x2u)

static int run(int access) { return a20_fault_from_status_rust(T, MM, VA, STVAL, access); }

static void declines(void)
{
    /* No page table to walk: nothing was opened, nothing is closed. */
    model_reset();
    open_rc = 0;
    check(run(MM_FAULT_ACCESS_READ) == 0, "decline: no page table declines");
    check(close_calls == 0 && alloc_calls == 0, "decline: nothing to close or allocate");
    check(flush_calls == 0 && bump_calls == 0, "decline: no accounting on a decline");

    /* An entry that is already backed is not ours to fill. */
    model_reset();
    open_already = 1;
    check(run(MM_FAULT_ACCESS_READ) == 0, "decline: an already-backed entry declines");
    check(close_calls == 1, "decline: the cursor is closed");
    check(alloc_calls == 0 && charge_calls == 0, "decline: nothing is allocated");

    /* Not a virtually-allocated anon entry. */
    model_reset();
    open_cls = (uint8_t)((MM_ST_ANON_MAPPED << 4) | MM_ST_PROT_R);
    check(run(MM_FAULT_ACCESS_READ) == 0, "decline: a mapped class declines");
    check(close_calls == 1 && alloc_calls == 0, "decline: the cursor is closed, nothing allocated");

    /* A userfaultfd registration wins over satisfying the fault. */
    model_reset();
    open_uffd = 1;
    check(run(MM_FAULT_ACCESS_READ) == 0, "decline: a uffd mark declines");
    check(close_calls == 1 && alloc_calls == 0, "decline: the uffd entry is left alone");

    /* Allocation failure. */
    model_reset();
    alloc_rc = PFN_NONE;
    check(run(MM_FAULT_ACCESS_READ) == 0, "decline: an allocation failure declines");
    check(charge_calls == 0 && close_calls == 1, "decline: no charge, cursor closed");

    /* Charge failure gives the frame back. */
    model_reset();
    charge_rc = -1;
    check(run(MM_FAULT_ACCESS_READ) == 0, "decline: a charge failure declines");
    check(frame_puts == 1, "decline: the frame is given back");
    check(zero_calls == 0 && map_calls == 0, "decline: nothing is zeroed or mapped");
    check(close_calls == 1, "decline: the cursor is closed");

    /* A lost install gives the charge and the frame back. */
    model_reset();
    map_rc = -1;
    check(run(MM_FAULT_ACCESS_READ) == 0, "decline: a lost install declines");
    check(uncharge_calls == 1 && frame_puts == 1, "decline: the charge and the frame are undone");
    check(close_calls == 1 && rss_calls == 0 && bump_calls == 0,
          "decline: a lost install is not accounted");
}

static void access_check(void)
{
    const uint8_t cls_ro = (uint8_t)((MM_ST_ANON_VIRT << 4) | MM_ST_PROT_R);
    const uint8_t cls_rw = (uint8_t)((MM_ST_ANON_VIRT << 4) | MM_ST_PROT_R | MM_ST_PROT_W);
    const uint8_t cls_x = (uint8_t)((MM_ST_ANON_VIRT << 4) | MM_ST_PROT_X);

    /* Read-only mark: a write and an exec fault must both decline before any
     * allocation, and the read must be served. */
    model_reset();
    open_cls = cls_ro;
    check(run(MM_FAULT_ACCESS_WRITE) == 0, "access: a write on a read-only mark declines");
    check(alloc_calls == 0, "access: the write decline happens before the allocation");
    check(close_calls == 1, "access: the write decline closes the cursor");

    model_reset();
    open_cls = cls_ro;
    check(run(MM_FAULT_ACCESS_EXEC) == 0, "access: an exec on a read-only mark declines");

    model_reset();
    open_cls = cls_ro;
    check(run(MM_FAULT_ACCESS_READ) == 1, "access: a read on a read-only mark is served");
    check(prot_calls == 1 && last_prot == PROT_READ, "access: the prot round-trips to READ");

    /* A write mark also satisfies a read fault (the VMA path allows it). */
    model_reset();
    open_cls = cls_rw;
    check(run(MM_FAULT_ACCESS_READ) == 1, "access: a write mark serves a read fault");
    check(last_prot == (PROT_READ | PROT_WRITE), "access: both prot bits round-trip");

    model_reset();
    open_cls = cls_rw;
    check(run(MM_FAULT_ACCESS_WRITE) == 1, "access: a write mark serves a write fault");

    /* An exec mark: only exec is allowed, and a read on it declines. */
    model_reset();
    open_cls = cls_x;
    check(run(MM_FAULT_ACCESS_EXEC) == 1, "access: an exec mark serves an exec fault");
    check(last_prot == PROT_EXEC, "access: the exec bit round-trips");
    model_reset();
    open_cls = cls_x;
    check(run(MM_FAULT_ACCESS_READ) == 0, "access: a read on an exec-only mark declines");
}

static void success(void)
{
    const uint8_t cls_rw = (uint8_t)((MM_ST_ANON_VIRT << 4) | MM_ST_PROT_R | MM_ST_PROT_W);
    model_reset();
    open_cls = cls_rw;
    alloc_rc = 0x2000;
    check(run(MM_FAULT_ACCESS_READ) == 1, "success: the fault is served");
    check(zero_calls == 1, "success: the new frame is zeroed");
    check(map_calls == 1, "success: the frame is installed");
    check(map_pa == ((uintptr_t)0x2000 << 12), "success: the install uses the new frame");
    check(map_allow == (0x1000u | (PROT_READ | PROT_WRITE)),
          "success: the install uses the architecture's flag set for the prot");
    check(map_cls == MM_ST_ANON_MAPPED, "success: the entry is published as anon-mapped");
    check(map_va_ok == 1, "success: the install targets the faulting page");
    check(close_calls == 1 && g_open_depth == 0, "success: the cursor is closed");
    check(rss_calls == 1 && bump_calls == 1, "success: rss and the counters are bumped");
    check(close_before_rss == 0, "success: the accounting runs after the cursor is closed");
    check(rss_before_bump == 1, "success: rss is added before the counters");
    check(flush_calls == 1 && last_flush_stval == STVAL,
          "success: the local TLB entry for the faulting address is flushed");
    check(frame_puts == 0, "success: nothing is released on the success path");
    check(charge_calls == 1 && uncharge_calls == 0, "success: the charge is kept");
    check(alloc_while_open == 1 && map_while_open == 1,
          "success: the allocation and the install run with the cursor open");

    /* Every path leaves the cursor closed exactly once. */
    model_reset();
    open_cls = cls_rw;
    run(MM_FAULT_ACCESS_READ);
    check(close_calls == 1 && g_open_depth == 0, "success: one close per call");
}

int main(void)
{
    declines();
    access_check();
    success();

    if (failures) {
        printf("FAULT_STATUS_HOST: FAIL %d/%d checks\n", failures, checks);
        return 1;
    }
    printf("FAULT_STATUS_HOST: PASS %d checks\n", checks);
    return 0;
}
