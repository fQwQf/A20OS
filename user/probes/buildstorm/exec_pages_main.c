typedef unsigned long size_t;

extern unsigned long a20_probe_dso_walk(unsigned long seed);

#define SYS_WRITE 64

static long raw_write(int fd, const void *buffer, size_t length)
{
#if defined(__riscv)
    register long a0 __asm__("a0") = fd;
    register long a1 __asm__("a1") = (long)buffer;
    register long a2 __asm__("a2") = (long)length;
    register long a7 __asm__("a7") = SYS_WRITE;
    __asm__ volatile("ecall"
                     : "+r"(a0)
                     : "r"(a1), "r"(a2), "r"(a7)
                     : "memory");
    return a0;
#elif defined(__loongarch__)
    register long a0 __asm__("$a0") = fd;
    register long a1 __asm__("$a1") = (long)buffer;
    register long a2 __asm__("$a2") = (long)length;
    register long a7 __asm__("$a7") = SYS_WRITE;
    __asm__ volatile("syscall 0"
                     : "+r"(a0)
                     : "r"(a1), "r"(a2), "r"(a7)
                     : "memory");
    return a0;
#endif
}

int probe_main(void)
{
    static const char ok[] = "BUILDSTORM_PROBE_DYNAMIC_PIE_DSO_OK\n";
    static const char fail[] = "BUILDSTORM_PROBE_DYNAMIC_PIE_DSO_FAIL\n";
    unsigned long result = a20_probe_dso_walk(0x2026UL);

    if (result != (0x2026UL ^ 0xa20b17d50UL)) {
        raw_write(1, fail, sizeof(fail) - 1);
        return 1;
    }
    raw_write(1, ok, sizeof(ok) - 1);
    return 0;
}
