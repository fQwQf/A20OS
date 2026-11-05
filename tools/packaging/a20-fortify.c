/*
 * glibc fortify-source stand-ins for musl guests.
 *
 * Only the riscv64 build of JNA's libjnidispatch.so (jna-5.17.0.jar) was
 * compiled with _FORTIFY_SOURCE: it imports __snprintf_chk and friends
 * @GLIBC_2.27, which Alpine's musl does not export.  The guest loader does
 * not fail loudly on those JUMP_SLOTs -- it leaves the lazy-binding stub in
 * the GOT, so the first fortified call jumps into the PLT resolver header
 * and dies with SIGSEGV at a bare ".plt" address (observed: MC on riscv64,
 * pc=0x3d80, right after Native.initIDs).  The x86_64 and aarch64 builds of
 * the same jar have no fortify references, which is why only riscv64 hits
 * this.
 *
 * tools/pkg.py compiles this per-arch into the GUI media overlay as
 * /usr/lib/a20/liba20-fortify.so; the guest launcher preloads it so the
 * symbols exist in the global namespace before JNA is loaded.  Signatures
 * match glibc's fortified entry points.  Where glibc would abort on a
 * detected overflow, these clamp or abort the same way; where glibc probes
 * the caller's buffer size and finds it unknown ((size_t)-1), the check is
 * skipped exactly as glibc skips it.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNKNOWN_SIZE ((size_t)-1)

int __snprintf_chk(char *str, size_t maxlen, int flag, size_t slen,
                   const char *format, ...)
{
    (void)flag;
    if (slen != UNKNOWN_SIZE && slen < maxlen)
        abort();                       /* glibc __chk_fail: caller lied */
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(str, maxlen, format, ap);
    va_end(ap);
    return n;
}

int __sprintf_chk(char *str, int flag, size_t slen, const char *format, ...)
{
    (void)flag;
    va_list ap;
    va_start(ap, format);
    /* slen is the destination object size; -1 means the caller's buffer was
     * not measurable and glibc performs no bound either. */
    int n = (slen == UNKNOWN_SIZE)
        ? vsprintf(str, format, ap)
        : vsnprintf(str, slen, format, ap);
    va_end(ap);
    return n;
}

int __fprintf_chk(FILE *stream, int flag, const char *format, ...)
{
    (void)flag;
    va_list ap;
    va_start(ap, format);
    int n = vfprintf(stream, format, ap);
    va_end(ap);
    return n;
}

void *__memcpy_chk(void *dest, const void *src, size_t len, size_t destlen)
{
    if (len > destlen)
        abort();                       /* glibc __chk_fail: caller lied */
    return memcpy(dest, src, len);
}

void __longjmp_chk(jmp_buf env, int val)
{
    /* glibc's variant refuses longjmps that would leave the current stack
     * frame; the plain call matches pre-fortify behaviour. */
    longjmp(env, val ? val : 1);
}
