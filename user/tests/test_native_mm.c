/*
 * Native ABI memory (VMO/VMAR) smoke test.
 *
 * Exercises the core-backed VMO paths end to end:
 *   1. vm_alloc: anonymous VMO mapping with lazy demand faults
 *   2. vm_remap: grow a mapping and verify data survives the copy
 *   3. vm_protect: change protection
 *   4. vm_advise(MADV_DONTNEED): drop PTEs without faulting
 *   5. vm_unmap: release the VMO
 *   6. VMAR reservations (hierarchy, ceilings, capability stamping)
 *   7. file-backed mapping through the page cache
 *   8. vm_advise(MADV_NORMAL): a pure hint must not drop pages
 *   9. vm_advise fork-policy advices and the range/error contract
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

    /* Closed VMAR handles must not remove a mapping's placement boundary.
     * Failed growth/reprotection must preserve the source and its data. */
    {
        const uint64_t region = 0x200000000ULL;
        a20_handle_t owner = A20_HANDLE_NULL;
        st = a20_vm_create_vmar(A20_HANDLE_NULL, region, 6 * PAGE,
                    A20_VMAR_CAN_MAP_READ | A20_VMAR_CAN_MAP_WRITE |
                    A20_VMAR_CAN_MAP_SPECIFIC, &owner);
        if (st != A20_OK) return fail(out, 64, "remap owner create", 18);
        a20_status_t backing = a20_vm_create_object(6 * PAGE, 0);
        a20_status_t block = a20_vm_create_object(4 * PAGE, 0);
        if (backing < 0 || block < 0) return fail(out, 65, "remap backing", 13);
        uint64_t source = 0, blocker = 0;
        st = a20_vm_map_in_vmar(owner, (a20_handle_t)backing, region,
                    2 * PAGE, 0, 3, 0x10, &source);
        if (st != A20_OK || source != region)
            return fail(out, 66, "remap source map", 16);
        st = a20_vm_map_in_vmar(owner, (a20_handle_t)block, region + 2 * PAGE,
                    4 * PAGE, 0, 3, 0x10, &blocker);
        if (st != A20_OK || blocker != region + 2 * PAGE)
            return fail(out, 67, "remap blocker map", 17);
        volatile uint64_t *first = (void *)(uintptr_t)source;
        first[0] = 0x123456789abcdef0ULL;
        first[PAGE / 8] = 0xfedcba9876543210ULL;
        *(volatile uint64_t *)(uintptr_t)blocker = 0x987654321ULL;
        /* Exercise owner reference duplication and re-merge during protect. */
        if (a20_vm_protect(source, PAGE, 1) != A20_OK ||
            a20_vm_protect(source, PAGE, 3) != A20_OK)
            return fail(out, 68, "remap split owner", 17);
        a20_hdl_close(owner);
        a20_hdl_close((a20_handle_t)backing);
        a20_hdl_close((a20_handle_t)block);
        st = a20_syscall6(A20_SYS_vm_remap, source, 2 * PAGE, 3 * PAGE, 0, 0, 0);
        if (st != -A20_ERR_NO_MEMORY || first[0] != 0x123456789abcdef0ULL ||
            first[PAGE / 8] != 0xfedcba9876543210ULL ||
            *(volatile uint64_t *)(uintptr_t)blocker != 0x987654321ULL)
            return fail(out, 69, "remap escaped owner", 19);
        st = a20_syscall6(A20_SYS_vm_remap, source, 2 * PAGE, 2 * PAGE, 4, 0, 0);
        if (st != -A20_ERR_ACCESS)
            return fail(out, 70, "remap reprotect rollback", 23);
        first[0] = 0x123456789abcdef0ULL; /* still writable after failure */
        st = a20_syscall6(A20_SYS_vm_remap, source, 2 * PAGE, 2 * PAGE, 1, 0, 0);
        if (st < 0 || (uint64_t)st != source ||
            a20_vm_protect(source, 2 * PAGE, 3) != A20_OK)
            return fail(out, 76, "remap in-place protect", 22);
        /* Keep one blocker page so in-place growth is impossible, and leave
         * a three-page gap inside the original owner for a genuine move. */
        if (a20_vm_unmap(blocker + PAGE, 3 * PAGE) != A20_OK)
            return fail(out, 71, "remap free blocker", 18);
        st = a20_syscall6(A20_SYS_vm_remap, source, 2 * PAGE, 3 * PAGE,
                          1, blocker + PAGE, 0);
        if (st < 0 || (uint64_t)st != blocker + PAGE)
            return fail(out, 72, "remap bounded move", 18);
        uint64_t moved = (uint64_t)st;
        volatile uint64_t *data = (void *)(uintptr_t)moved;
        if (data[0] != 0x123456789abcdef0ULL ||
            data[PAGE / 8] != 0xfedcba9876543210ULL || data[2 * PAGE / 8] != 0 ||
            a20_vm_protect(source, PAGE, 1) != -A20_ERR_NO_MEMORY ||
            a20_vm_protect(moved, 3 * PAGE, 3) != A20_OK)
            return fail(out, 73, "remap data/ceiling", 18);
        st = a20_syscall6(A20_SYS_vm_remap, moved, 3 * PAGE, 4 * PAGE, 0, 0, 0);
        if (st != -A20_ERR_NO_MEMORY || data[0] != 0x123456789abcdef0ULL)
            return fail(out, 74, "remap lost placement", 20);
        if (a20_vm_unmap(moved, 3 * PAGE) != A20_OK ||
            a20_vm_unmap(blocker, PAGE) != A20_OK)
            return fail(out, 75, "remap final unmap", 17);
        a20_hdl_write_buf(out, "NATIVE_MM: remap-owner-rollback PASS\n",
                          sizeof("NATIVE_MM: remap-owner-rollback PASS\n") - 1, (void *)0);
    }

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
        a20_handle_t ov = A20_HANDLE_NULL;
        st = a20_vm_create_vmar(root, 0x100800000ULL, 16 * 1024 * 1024ULL,
                                R | W, &ov);
        if (st == A20_OK)
            return fail(out, 11, "vmar overlap allowed", 21);
        a20_hdl_close(ov);

        /* Map a VMO through the child at a fixed address: WRITE exceeds the
         * child ceiling -> ACCESS; read-only fits. */
        a20_status_t created = a20_vm_create_object(2 * PAGE, 0);
        if (created < 0)
            return fail(out, 12, "vm_create_object failed", 23);
        a20_handle_t vmo = (a20_handle_t)created;

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

        /* Reject an out-of-region MAP_FIXED before it can destroy another
         * mapping. The victim is allocated outside the reserved window. */
        uint64_t victim = 0;
        st = a20_vm_alloc_pages(1, 3, &victim);
        if (st != A20_OK || !victim)
            return fail(out, 60, "victim alloc", 12);
        *(volatile uint64_t *)(uintptr_t)victim = 0x123456789abcdef0ULL;
        uint64_t escaped = 0;
        st = a20_vm_map_in_vmar(child, vmo, victim, PAGE, 0, 1,
                                0x10 /* MAP_FIXED */, &escaped);
        if (st == A20_OK || *(volatile uint64_t *)(uintptr_t)victim !=
                               0x123456789abcdef0ULL)
            return fail(out, 61, "fixed escape damaged victim", 27);
        a20_vm_unmap(victim, PAGE);

        uint64_t guard = 0;
        st = a20_vm_map(vmo, PAGE, 0, 0 /* NONE */, &guard);
        if (st != A20_OK || !guard ||
            a20_vm_protect(guard, PAGE, 1) != -A20_ERR_ACCESS)
            return fail(out, 62, "zero cap not enforced", 21);
        a20_vm_unmap(guard, PAGE);
        a20_vm_unmap(maddr, 2 * PAGE);

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
        po.path = (uint64_t)(uintptr_t)A20_NATIVE_BIN("native-mm");
        po.path_len = sizeof(A20_NATIVE_BIN("native-mm")) - 1;
        /* A Native open/close must not leave a hidden Linux fd behind. */
        for (unsigned i = 0; i < 1024; ++i) {
            st = a20_path_open(&po);
            if (st != A20_OK)
                return fail(out, 63, "path_open leaked fds", 20);
            a20_hdl_close(po.out_handle);
        }
        st = a20_path_open(&po);
        if (st != A20_OK || po.out_handle == A20_HANDLE_NULL)
            return fail(out, 16, "path_open failed", 16);

        uint64_t faddr = 0;
        st = a20_vm_map(po.out_handle, PAGE, 0, 1 /* R */, &faddr);
        if (st != A20_OK || !faddr)
            return fail(out, 17, "vm_map FILE failed", 18);

        /* The VMA must retain its own file reference after the source handle
         * closes; a subsequent fault still needs the backing vfile. */
        a20_hdl_close(po.out_handle);
        po.out_handle = A20_HANDLE_NULL;

        volatile const uint8_t *fb = (volatile const uint8_t *)(uintptr_t)faddr;
        if (!(fb[0] == 0x7f && fb[1] == 'E' && fb[2] == 'L' && fb[3] == 'F'))
            return fail(out, 18, "file map content", 18);

        st = a20_vm_unmap(faddr, PAGE);
        if (st != A20_OK)
            return fail(out, 19, "vm_unmap FILE", 14);
    }

    /* 8. vm_advise(MADV_NORMAL): a pure hint.  It must return success and
     *    leave the pages in place -- MADV_NORMAL means "expect normal
     *    access", and a kernel that treated it as a discard would hand the
     *    caller a range whose contents are gone. */
    {
        uint64_t naddr = 0;
        st = a20_vm_alloc_pages(2, 3 /* PROT_R|PROT_W */, &naddr);
        if (st != A20_OK || !naddr)
            return fail(out, 20, "vm_alloc NORMAL", 15);

        volatile uint8_t *nb = (volatile uint8_t *)(uintptr_t)naddr;
        for (uint64_t i = 0; i < 2 * PAGE; i++)
            nb[i] = (uint8_t)(0xa5 ^ (i & 0xff));

        st = a20_syscall6(A20_SYS_vm_advise, naddr, 2 * PAGE,
                          0 /* MADV_NORMAL */, 0, 0, 0);
        if (st != A20_OK)
            return failh(out, 21, "vm_advise NORMAL st=", 20, st);
        for (uint64_t i = 0; i < 2 * PAGE; i++) {
            if (nb[i] != (uint8_t)(0xa5 ^ (i & 0xff)))
                return fail(out, 22, "MADV_NORMAL dropped", 19);
        }

        st = a20_vm_unmap(naddr, 2 * PAGE);
        if (st != A20_OK)
            return fail(out, 23, "vm_unmap NORMAL", 15);
    }

    /* 9. The fork-policy advices are the only ones that change kernel state,
     *    so each of the four gets a round trip over a mapped range.  What the
     *    child ends up with is not checkable from here -- this is a single
     *    process and there is no fork in the Native ABI test -- but the
     *    dispatch reaching the VMA flag path at all is, as is the range
     *    contract around it. */
    {
        uint64_t kaddr = 0;
        st = a20_vm_alloc_pages(3, 3 /* PROT_R|PROT_W */, &kaddr);
        if (st != A20_OK || !kaddr)
            return fail(out, 24, "vm_alloc fork", 13);

        static const int fork_adv[] = {
            10, /* MADV_DONTFORK */
            11, /* MADV_DOFORK */
            18, /* MADV_WIPEONFORK */
            19, /* MADV_KEEPONFORK */
        };
        for (unsigned i = 0; i < sizeof(fork_adv) / sizeof(fork_adv[0]); i++) {
            st = a20_syscall6(A20_SYS_vm_advise, kaddr, 3 * PAGE,
                              (uint64_t)fork_adv[i], 0, 0, 0);
            if (st != A20_OK)
                return failh(out, 25, "fork advise st=", 15, st);
        }

        /* A range running past the mapping is refused, for a fork bit and for
         * a hint alike: the hint path validates coverage too, so the same
         * address answers the same way whichever advice it was given. */
        st = a20_syscall6(A20_SYS_vm_advise, kaddr, 8 * PAGE,
                          10 /* MADV_DONTFORK */, 0, 0, 0);
        if (st != -A20_ERR_NO_MEMORY)
            return failh(out, 26, "fork hole st=", 13, st);
        st = a20_syscall6(A20_SYS_vm_advise, kaddr, 8 * PAGE,
                          0 /* MADV_NORMAL */, 0, 0, 0);
        if (st != -A20_ERR_NO_MEMORY)
            return failh(out, 27, "hint hole st=", 13, st);

        /* addr+len that wraps is a rejected range, not an empty success. */
        st = a20_syscall6(A20_SYS_vm_advise, 0xfffffffffffff000ULL, 0x2000,
                          4 /* MADV_DONTNEED */, 0, 0, 0);
        if (st != -A20_ERR_INVALID_ARGUMENT)
            return failh(out, 28, "wrap range st=", 14, st);

        /* An advice value the kernel does not know is refused rather than
         * taken for a hint. */
        st = a20_syscall6(A20_SYS_vm_advise, kaddr, PAGE, 999, 0, 0, 0);
        if (st != -A20_ERR_INVALID_ARGUMENT)
            return failh(out, 29, "unknown advice st=", 18, st);

        /* MADV_REMOVE has no swapout to promise with, so it says so instead
         * of discarding the range the caller expects to read back. */
        st = a20_syscall6(A20_SYS_vm_advise, kaddr, PAGE, 9 /* MADV_REMOVE */,
                          0, 0, 0);
        if (st != -A20_ERR_NOT_SUPPORTED)
            return failh(out, 30, "MADV_REMOVE st=", 15, st);

        st = a20_vm_unmap(kaddr, 3 * PAGE);
        if (st != A20_OK)
            return fail(out, 31, "vm_unmap fork", 13);
    }

    a20_hdl_write_buf(out, "NATIVE_MM: PASS\n", 16, (void *)0);
    return 0;
}
