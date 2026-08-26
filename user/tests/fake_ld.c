/*
 * Minimal PT_INTERP interpreter stand-in ("fakeld") — dynamic-entry
 * bring-up probe for docs/native-abi/08-runtime-status.md §8a.
 *
 * Kernel contract for native-dynamic tasks (kernel/mm/elf.c):
 *   - app and interpreter are both pre-mapped; entry is the interp's
 *   - a0 = &a20_start_info_t (placed just below the stack top)
 *   - sp -> [argc][argv..NULL][envp..NULL][auxv pairs .. AT_NULL]
 *   - auxv carries AT_BASE(interp base), AT_ENTRY(real entry),
 *     AT_PHDR(app phdr table), AT_PAGESZ ...
 *
 * fakeld validates that contract end-to-end, then hands control to the
 * real entry with a0 forwarded (liba20rt crt0 expects start_info there).
 */
#include "liba20rt/a20_types.h"
#include "liba20rt/a20_syscall.h"
#include "liba20rt/a20_handle.h"

#define AT_NULL    0u
#define AT_PHDR    3u
#define AT_PAGESZ  6u
#define AT_BASE    7u
#define AT_ENTRY   9u

static a20_handle_t g_out;

static void emit(const char *s, uint32_t n)
{
    if (g_out != A20_HANDLE_NULL)
        a20_hdl_write_buf(g_out, s, n, (void *)0);
}

static void emit_hex(const char *tag, uint32_t taglen, uint64_t v)
{
    char b[18];
    emit(tag, taglen);
    b[0] = '0';
    b[1] = 'x';
    for (int i = 17; i >= 2; i--) {
        unsigned nib = (unsigned)(v & 0xf);
        b[i] = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
        v >>= 4;
    }
    emit(b, 18);
}

void dyn_main(uint64_t si_va, uint64_t spvec)
{
    const volatile a20_start_info_t *si =
        (const volatile a20_start_info_t *)si_va;
    g_out = si ? si->stdout_handle : A20_HANDLE_NULL;
    if (g_out == A20_HANDLE_NULL && si)
        g_out = si->stderr_handle;
    emit("FAKELD: enter\n", 14);

    uint64_t argc = *(const volatile uint64_t *)spvec;
    const uint64_t *p = (const uint64_t *)spvec + 1;
    p += argc + 1;                       /* argv vector + NULL */
    uint32_t guard = 4096;
    while (*p && guard--) p++;           /* envp */
    if (!guard) { emit("FAKELD: bad stack\n", 18); goto die; }
    p++;                                 /* envp NULL */

    uint64_t at_base = 0, at_entry = 0, at_phdr = 0, at_pagesz = 0;
    for (uint32_t i = 0; i < 128; i++, p += 2) {
        uint64_t k = p[0], v = p[1];
        if (k == AT_NULL) break;
        switch (k) {
        case AT_BASE:   at_base = v; break;
        case AT_ENTRY:  at_entry = v; break;
        case AT_PHDR:   at_phdr = v; break;
        case AT_PAGESZ: at_pagesz = v; break;
        default: break;
        }
    }

    int ok = at_pagesz == 4096 && at_base && at_entry && at_phdr;
    if (ok) {
        /* AT_PHDR targets the program-header table inside the pre-mapped
         * app image: require the pointer to resolve to readable memory
         * holding a non-zero first p_type. */
        const volatile uint32_t *ph = (const volatile uint32_t *)at_phdr;
        ok = ph[0] != 0;
        emit("FAKELD: hdr0=", 12);
        {
            char hb[8];
            for (int i = 0; i < 8; i++) {
                unsigned nib = ((unsigned)ph[0] >> ((7 - i) * 4)) & 0xf;
                hb[i] = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
            }
            emit(hb, 8);
        }
        emit("\n", 1);
    }

    emit("FAKELD:", 7);
    emit_hex(" base=", 6, at_base);
    emit_hex(" entry=", 7, at_entry);
    emit_hex(" phdr=", 6, at_phdr);
    emit(ok ? " ok\n" : " bad\n", ok ? 4 : 5);

    if (ok)
        ((void (*)(uint64_t))at_entry)(si_va);

die:
    a20_syscall6(A20_SYS_task_exit, ok ? 126 : 125, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}

/* Entry: the kernel passes a0 = &start_info; forward sp as second arg. */
__asm__(
    ".text\n"
    ".globl _start_dyn\n"
    "_start_dyn:\n"
    "    mv a1, sp\n"
    "    tail dyn_main\n");
