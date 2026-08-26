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
#include "liba20rt/a20_fs.h"

#define PAGE 4096ULL

static int fail(a20_handle_t out, int code, const char *msg, uint32_t len);

static void put_hex(a20_handle_t out, uint64_t v)
{
    char b[17];
    for (int i = 15; i >= 0; i--) {
        uint8_t n = (uint8_t)(v & 0xf);
        b[i] = n < 10 ? (char)('0' + n) : (char)('a' + n - 10);
        v >>= 4;
    }
    b[16] = 0;
    a20_hdl_write_buf(out, b, 16, (void *)0);
}

static int failh(a20_handle_t out, int code, const char *msg, uint32_t len,
                 a20_status_t st)
{
    fail(out, code, msg, len);
    put_hex(out, (uint64_t)st);
    a20_hdl_write_buf(out, "\n", 1, (void *)0);
    return code;
}

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

    /* 6. VMAR reservations (04-memory §3): hierarchy, monotonic ceilings,
     *    containment and cap stamping. */
    {
        const uint64_t R = A20_VMAR_CAN_MAP_READ;
        const uint64_t W = A20_VMAR_CAN_MAP_WRITE;
        const uint64_t S = A20_VMAR_CAN_MAP_SPECIFIC;

        /* Root over a 64 MiB window inside the user range. */
        a20_handle_t root;
        st = a20_vm_create_vmar(A20_HANDLE_NULL, 0x100000000ULL,
                                64 * 1024 * 1024ULL, R | W | S, &root);
        if (st != A20_OK)
            return fail(out, 9 + (int)(-st), "vmar root failed", 16);

        /* Child narrowed to read-only (fixed-address maps still allowed),
         * first quarter of the parent. */
        a20_handle_t child;
        st = a20_vm_create_vmar(root, 0x100000000ULL, 16 * 1024 * 1024ULL,
                                R | S, &child);
        if (st != A20_OK)
            return failh(out, 10, "vmar child st=", 14, st);

        /* Overlap must be refused. */
        a20_handle_t ov;
        st = a20_vm_create_vmar(root, 0x10800000ULL, 16 * 1024 * 1024ULL,
                                R | W, &ov);
        if (st == A20_OK)
            return fail(out, 11, "vmar overlap allowed", 21);
        a20_hdl_close(ov);

        /* Map a VMO through the child at a fixed address: WRITE exceeds the
         * child ceiling -> ACCESS; read-only fits. */
        a20_handle_t vmo = a20_vm_create_object(2 * PAGE, 0);
        if (vmo < 0)
            return fail(out, 12, "vm_create_object failed", 23);

        uint64_t maddr = 0;
        st = a20_vm_map_in_vmar(child, vmo, 0x100010000ULL, 2 * PAGE, 0,
                                3 /* R|W */, 0x10 /* MAP_FIXED */, &maddr);
        if (st == A20_OK)
            return fail(out, 13, "vmar cap not enforced", 22);

        st = a20_vm_map_in_vmar(child, vmo, 0x100010000ULL, 2 * PAGE, 0,
                                1 /* R */, 0x10 /* MAP_FIXED */, &maddr);
        if (st != A20_OK || maddr != 0x100010000ULL)
            return failh(out, 14, "vmar map st=", 12, st);

        /* The stamped cap survives protect: raising to RW is refused. */
        st = a20_vm_protect(maddr, 2 * PAGE, 3 /* R|W */);
        if (st == A20_OK)
            return fail(out, 15, "vmar stamp lost", 17);

        a20_hdl_close(vmo);
        a20_hdl_close(child);
        a20_hdl_close(root);
    }

    /* 7. FILE-backed mapping: demand fault fills pages via page cache. */
    {
        a20_path_open_args_t po;
        a20_memset(&po, 0, sizeof(po));
        po.dir = A20_HANDLE_NULL;
        po.flags = A20_PATH_OPEN_RDONLY;
        po.rights = A20_RIGHT_READ | A20_RIGHT_MAP;
        po.path = (uint64_t)(uintptr_t)"/bin/native-mm-rv";
        po.path_len = 17;
        st = a20_path_open(&po);
        if (st != A20_OK || po.out_handle == A20_HANDLE_NULL)
            return fail(out, 16, "path_open failed", 16);

        uint64_t faddr = 0;
        st = a20_vm_map(po.out_handle, PAGE, 0, 1 /* R */, &faddr);
        if (st != A20_OK || !faddr)
            return fail(out, 17, "vm_map FILE failed", 18);

        volatile const uint8_t *fb = (volatile const uint8_t *)(uintptr_t)faddr;
        if (!(fb[0] == 0x7f && fb[1] == 'E' && fb[2] == 'L' && fb[3] == 'F'))
            return fail(out, 18, "file map content", 18);

        st = a20_vm_unmap(faddr, PAGE);
        if (st != A20_OK)
            return fail(out, 19, "vm_unmap FILE", 14);
        a20_hdl_close(po.out_handle);
    }

    a20_hdl_write_buf(out, "NATIVE_MM: PASS\n", 16, (void *)0);
    return 0;
}
