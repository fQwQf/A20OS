/*
 * Native ABI memory (VMO/VMAR) smoke test.
 *
 * Exercises the core-backed VMO paths end to end:
 *   1. vm_alloc: anonymous VMO mapping with lazy demand faults
 *   2. vm_remap: grow a mapping and verify data survives the copy
 *   3. vm_protect: change protection
 *   4. vm_advise(MADV_DONTNEED): drop PTEs without faulting
 *   5. vm_unmap: release the VMO
 */
#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"
#include "liba20rt/a20_mem.h"
#include "liba20rt/a20_syscall.h"

#define PAGE 4096ULL

static int fail(a20_handle_t out, int code, const char *msg, uint32_t len)
{
    a20_hdl_write_buf(out, "NATIVE_MM: FAIL ", 16, (void *)0);
    a20_hdl_write_buf(out, msg, len, (void *)0);
    a20_hdl_write_buf(out, "\n", 1, (void *)0);
    return code;
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    a20_start_info_t *si = a20_get_start_info();
    a20_handle_t out = si ? si->stdout_handle : A20_HANDLE_NULL;
    if (out == A20_HANDLE_NULL)
        return 90;

    /* 1. vm_alloc: 16 pages, RW. */
    uint64_t base = 0;
    a20_status_t st = a20_vm_alloc_pages(16, 3 /* PROT_R|PROT_W */, &base);
    if (st != A20_OK || !base)
        return fail(out, 1, "vm_alloc failed", 15);

    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)base;
    for (uint64_t i = 0; i < 16 * PAGE; i++)
        p[i] = (uint8_t)(i & 0xff);
    for (uint64_t i = 0; i < 16 * PAGE; i++) {
        if (p[i] != (uint8_t)(i & 0xff))
            return fail(out, 2, "alloc data mismatch", 20);
    }

    /* 2. vm_remap: grow 16 -> 32 pages, old data must survive.  Returns the
     * new mapping address (>= 0) on success. */
    uint64_t new_addr = 0;
    st = a20_syscall6(A20_SYS_vm_remap, base, 16 * PAGE, 32 * PAGE,
                      3 /* PROT_R|W */, 0, 0);
    if (st < 0 || (uint64_t)st == 0)
        return fail(out, 3, "vm_remap failed", 15);
    new_addr = (uint64_t)st;
    volatile uint8_t *np = (volatile uint8_t *)(uintptr_t)new_addr;
    for (uint64_t i = 0; i < 16 * PAGE; i++) {
        if (np[i] != (uint8_t)(i & 0xff))
            return fail(out, 4, "remap data lost", 16);
    }
    /* New tail is demand-paged and zero-filled. */
    for (uint64_t i = 16 * PAGE; i < 32 * PAGE; i++) {
        if (np[i] != 0)
            return fail(out, 5, "remap tail not zero", 20);
    }
    base = new_addr;

    /* 3. vm_protect: RW -> R (no write). */
    st = a20_vm_protect(base, 32 * PAGE, 1 /* PROT_R */);
    if (st != A20_OK)
        return fail(out, 6, "vm_protect failed", 18);

    /* 4. vm_advise MADV_DONTNEED: must not fault or corrupt. */
    st = a20_syscall6(A20_SYS_vm_advise, base, 4 * PAGE, 4 /* MADV_DONTNEED */, 0, 0, 0);
    if (st != A20_OK)
        return fail(out, 7, "vm_advise failed", 17);

    /* 5. vm_unmap. */
    st = a20_vm_unmap(base, 32 * PAGE);
    if (st != A20_OK)
        return fail(out, 8, "vm_unmap failed", 16);

    a20_hdl_write_buf(out, "NATIVE_MM: PASS\n", 16, (void *)0);
    return 0;
}
