__attribute__((visibility("default"), noinline))
unsigned long a20_probe_dso_walk(unsigned long seed)
{
    /*
     * Execute enough architecture NOPs to cross many file-backed executable
     * pages.  The function lives in a separate DSO and cannot be optimized
     * into the PIE caller.
     */
    __asm__ volatile(
        ".rept 16384\n"
        "nop\n"
        ".endr\n"
        :
        :
        : "memory");
    return seed ^ 0xa20b17d50UL;
}
