#ifdef CONFIG_PPC64LE

#include "firmware.h"
#include "cpu.h"

#define H_GET_TERM_CHAR 0x54UL
#define H_PUT_TERM_CHAR 0x58UL

static long pseries_hcall(uint64_t opcode, uint64_t arg1, uint64_t arg2,
                          uint64_t arg3, uint64_t arg4, uint64_t *out0,
                          uint64_t *out1)
{
    register uint64_t r3 __asm__("r3") = opcode;
    register uint64_t r4 __asm__("r4") = arg1;
    register uint64_t r5 __asm__("r5") = arg2;
    register uint64_t r6 __asm__("r6") = arg3;
    register uint64_t r7 __asm__("r7") = arg4;
    register uint64_t r8 __asm__("r8");
    __asm__ __volatile__(
        "sc 1"
        : "+r"(r3), "+r"(r4), "+r"(r5), "+r"(r6), "+r"(r7), "=r"(r8)
        :
        : "r9", "r10", "r11", "r12", "ctr", "lr", "cr0", "memory");
    if (out0)
        *out0 = r4;
    if (out1)
        *out1 = r5;
    return (long)r3;
}

void arch_firmware_init(void)
{
}

#define VTERMNO 0x71000000UL

void firmware_console_putchar(char c)
{
    uint64_t ch = (uint8_t)c;
    uint64_t be_char_reg = __builtin_bswap64(ch);
    (void)pseries_hcall(H_PUT_TERM_CHAR, VTERMNO, 1, be_char_reg, 0, NULL, NULL);
}

int firmware_console_getchar(void)
{
    uint64_t count = 0;
    uint64_t ch = 0;
    if (pseries_hcall(H_GET_TERM_CHAR, VTERMNO, 0, 0, 0, &count, &ch) != 0)
        return -1;
    if (!count)
        return -1;
    return (int)(__builtin_bswap64(ch) & 0xffU);
}

void firmware_set_timer(uint64_t time)
{
    (void)time;
}

void firmware_shutdown(void)
{
    arch_halt();
}

void firmware_reboot(void)
{
    arch_halt();
}

void sbi_set_timer(uint64_t time)
{
    firmware_set_timer(time);
}

void sbi_console_putchar(char c)
{
    firmware_console_putchar(c);
}

int sbi_console_getchar(void)
{
    return firmware_console_getchar();
}

void sbi_shutdown(void)
{
    firmware_shutdown();
}

void sbi_reboot(void)
{
    firmware_reboot();
}

#endif /* CONFIG_PPC64LE */
