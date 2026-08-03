#ifdef CONFIG_PPC64LE

#include "firmware.h"
#include "cpu.h"
#include "core/stdio.h"

#define VTERMNO         0x71000000UL
#define H_GET_TERM_CHAR 0x54UL
#define H_PUT_TERM_CHAR 0x58UL
#define H_PUT_TCE       0x20UL
#define RTAS_TOKEN_POWER_OFF  0x2003U
#define RTAS_TOKEN_SYSTEM_REBOOT 0x2004U

#define PSERIES_PCI_LIOBN       0x80000000UL
#define PSERIES_DMA_WINDOW_SIZE 0x40000000UL
#define PSERIES_DMA_IOVA_BASE   0x00000000UL

static long pseries_hcall(uint64_t opcode, uint64_t arg1, uint64_t arg2,
                          uint64_t arg3, uint64_t arg4, uint64_t *out0,
                          uint64_t *out1, uint64_t *out2)
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
    if (out2)
        *out2 = r6;
    return (long)r3;
}

typedef struct {
    uint32_t token;
    uint32_t nargs;
    uint32_t nret;
    uint32_t data[4];
} ppc64_rtas_args_t;

static int ppc64_rtas_call(uint32_t token, uint32_t nargs, uint32_t nret,
                           const int32_t *inputs)
{
    static ppc64_rtas_args_t args __attribute__((aligned(64)));

    args.token = __builtin_bswap32(token);
    args.nargs = __builtin_bswap32(nargs);
    args.nret = __builtin_bswap32(nret);
    for (uint32_t i = 0; i < nargs && i < 4; i++)
        args.data[i] = __builtin_bswap32((uint32_t)inputs[i]);
    for (uint32_t i = nargs; i < 4; i++)
        args.data[i] = 0;

    uint64_t physical = (uint64_t)(uintptr_t)&args - PAGE_OFFSET;
    long hrc = pseries_hcall(0xf000UL, physical, 0, 0, 0,
                             NULL, NULL, NULL);
    if (hrc != 0)
        return (int)hrc;
    return (int)(int32_t)__builtin_bswap32(args.data[nargs]);
}

void arch_firmware_init(void)
{
    /*
     * Do not populate the whole TCE window here.  A 1 GiB identity map would
     * require 262144 H_PUT_TCE hypercalls before the first kernel subsystem
     * can run.  The current pSeries bring-up path does not enumerate a DMA
     * device, and the generic DMA API still uses physical addresses directly.
     * A bounded, device-aware TCE setup belongs in the PCI/DMA path.
     */
    (void)H_PUT_TCE;
    (void)PSERIES_PCI_LIOBN;
    (void)PSERIES_DMA_WINDOW_SIZE;
    (void)PSERIES_DMA_IOVA_BASE;
    /* Mask every XIVE external interrupt (the port only polls). */
    ppc64_xics_ack();
}

void firmware_console_putchar(char c)
{
    uint64_t ch = (uint8_t)c;
    uint64_t be_char_reg = __builtin_bswap64(ch);
    (void)pseries_hcall(H_PUT_TERM_CHAR, VTERMNO, 1, be_char_reg, 0,
                       NULL, NULL, NULL);
}

int firmware_console_getchar(void)
{
    static uint8_t input[16];
    static unsigned input_pos;
    static unsigned input_len;

    if (input_pos < input_len)
        return input[input_pos++];

    uint64_t count = 0;
    uint64_t chars0_7 = 0;
    uint64_t chars8_15 = 0;
    if (pseries_hcall(H_GET_TERM_CHAR, VTERMNO, 0, 0, 0,
                      &count, &chars0_7, &chars8_15) != 0)
        return -1;
    if (!count)
        return -1;

    if (count > sizeof(input))
        count = sizeof(input);
    chars0_7 = __builtin_bswap64(chars0_7);
    chars8_15 = __builtin_bswap64(chars8_15);
    unsigned first = count < 8 ? (unsigned)count : 8;
    for (unsigned i = 0; i < first; i++)
        input[i] = (uint8_t)(chars0_7 >> (i * 8));
    for (unsigned i = 8; i < count; i++)
        input[i] = (uint8_t)(chars8_15 >> ((i - 8) * 8));

    input_pos = 1;
    input_len = (unsigned)count;
    return input[0];
}

void firmware_set_timer(uint64_t time)
{
    uint64_t now = arch_read_cycle();
    uint64_t delta = time > now ? time - now : 1;

    /*
     * Book3S uses a signed 32-bit decrementer.  Once it becomes negative it
     * keeps requesting an interrupt until software reloads it.
     */
    if (delta > 0x7fffffffUL)
        delta = 0x7fffffffUL;
    __asm__ __volatile__("mtspr 22,%0" :: "r"(delta) : "memory");
}

void firmware_shutdown(void)
{
    static const int32_t args[] = { -1, -1 };
    (void)ppc64_rtas_call(RTAS_TOKEN_POWER_OFF, 2, 1, args);
    arch_halt();
}

void firmware_reboot(void)
{
    (void)ppc64_rtas_call(RTAS_TOKEN_SYSTEM_REBOOT, 0, 1, NULL);
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

/*
 * The pSeries machine is POWER10 XIVE (ov5 XIVE_EXPLOIT): the legacy XICS
 * hypercalls (H_XIRR/H_EOI) are not registered and there is no ICP MMIO.
 * entry.S maps the XIVE Thread Interrupt Management Area (TIMA) at VA
 * 0xC000800040000000; the OS/pool/HV rings live at +0x190000/+0x1A0000/
 * +0x1B0000 and their CPPR bytes at +0x011/+0x021/+0x031.
 */
#define PPC64_TIMA_VA      0xC000800040000000UL
#define PPC64_TIMA_OS_OFF  0x190000UL
#define PPC64_TIMA_POOL_OFF 0x1A0000UL
#define PPC64_TIMA_HV_OFF  0x1B0000UL
#define PPC64_TIMA_CPPR    0x011UL

/*
 * The XIVE Thread Interrupt Management Area (TIMA) pages are mapped at
 * 0xC000800040000000 with the OS (ring 1), pool (ring 2) and HV (ring 3)
 * pages at 0x190000, 0x1A0000 and 0x1B0000.  The Current Processor Priority
 * Register (CPPR) of each ring lives at +0x011/+0x021/+0x031.  Every driver
 * on this port polls and the decrementer is a separate exception, so raising
 * all CPPRs to 0xff masks every XIVE external interrupt.
 */
void ppc64_xics_ack(void)
{
    *(volatile uint8_t *)(PPC64_TIMA_VA + PPC64_TIMA_OS_OFF +
                          PPC64_TIMA_CPPR) = 0xff;
    *(volatile uint8_t *)(PPC64_TIMA_VA + PPC64_TIMA_POOL_OFF +
                          PPC64_TIMA_CPPR + 0x10) = 0xff;
    *(volatile uint8_t *)(PPC64_TIMA_VA + PPC64_TIMA_HV_OFF +
                          PPC64_TIMA_CPPR + 0x20) = 0xff;
}

void sbi_reboot(void)
{
    firmware_reboot();
}

#endif /* CONFIG_PPC64LE */
