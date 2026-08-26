/*
 * Dynamic-path probe: this binary carries PT_INTERP pointing at
 * /fakeld-rv.  Reaching main proves the whole chain — the kernel
 * detected PT_INTERP, loaded the interpreter, switched entry to it,
 * built conventional auxv (AT_BASE/AT_ENTRY/AT_PHDR), and fakeld
 * forwarded control back into this image via AT_ENTRY
 * (docs/native-abi/08-runtime-status.md §8a).
 */
#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"

/* The linker script declares the INTERP segment but ld does not
 * synthesize .interp contents — provide the interpreter path here. */
__asm__(
    ".section .interp,\"a\",@progbits\n"
    ".string \"/bin/fakeld-rv\"\n"
    ".previous\n");

int main(int argc, char **argv, char **envp)
{
    (void)argc;
    (void)argv;
    (void)envp;
    a20_start_info_t *si = a20_get_start_info();
    a20_handle_t out = si ? si->stdout_handle : A20_HANDLE_NULL;
    if (out == A20_HANDLE_NULL)
        return 90;
    a20_hdl_write_buf(out, "DYNPROBE: PASS\n", 15, (void *)0);
    return 0;
}
