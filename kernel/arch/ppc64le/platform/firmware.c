#ifdef CONFIG_PPC64LE

#include "firmware.h"
#include "cpu.h"

#define H_GET_TERM_CHAR 0x54UL
#define H_PUT_TERM_CHAR 0x58UL
#define H_PUT_TCE       0x20UL

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

void arch_firmware_init(void)
{
    /*
     * pseries PCI devices DMA through a TCE IOMMU.  Keep the driver's
     * existing physical-address DMA contract by installing a 1:1 map for
     * the whole 1 GiB guest window before any virtio queue is created.
     */
    /*
     * SLOF leaves a small identity range populated.  Replace it too: stale
     * translations there otherwise redirect low guest IOVAs into firmware
     * scratch memory after the kernel starts allocating from low RAM.
     */
    for (uint64_t pa = PSERIES_DMA_IOVA_BASE;
         pa < PSERIES_DMA_WINDOW_SIZE; pa += 4096) {
        register uint64_t r3 __asm__("r3") = H_PUT_TCE;
        register uint64_t r4 __asm__("r4") = PSERIES_PCI_LIOBN;
        register uint64_t r5 __asm__("r5") = pa;
        register uint64_t r6 __asm__("r6") = pa | 3UL;
        __asm__ __volatile__(
            "sc 1"
            : "+r"(r3), "+r"(r4), "+r"(r5), "+r"(r6)
            :
            : "r7", "r8", "r9", "r10", "r11", "r12",
              "ctr", "lr", "cr0", "memory");
        if (r3 != 0)
            break;
    }
}

#define VTERMNO 0x71000000UL

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
