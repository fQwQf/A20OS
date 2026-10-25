/* Register facts: BouffaloSDK 1cd1fd514df7ed0dfc1221be871f5aae03f31bbc,
 * drivers/soc/bl702/std/include/hardware/{glb,hbn}_reg.h and
 * drivers/lhal/include/hardware/uart_reg.h.  No vendor SDK code linked.
 * Probe only: no scheduler, PLIC, SBI, USB CDC or storage claims. */
#include <stdint.h>

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define GLB 0x40000000u
#define HBN 0x4000f000u
#define UART 0x4000a000u
#define UART_BAUD 115200u

volatile uint32_t probe_stage;
static volatile uint32_t bss_check[32];
static volatile uint32_t data_check = 0x41323050u;

static void uart_init(void) {
    /* Run from SRAM at RC32M; avoid DLL/XIP dependencies.  The ROM normally
     * leaves RC32M enabled.  Root clock and UART source become FCLK=32 MHz. */
    REG32(GLB) |= 0x0eu;                 /* FCLK/HCLK/BCLK enables */
    REG32(HBN + 0x30) &= ~7u;            /* RC32M root; UART FCLK */
    for (volatile unsigned n = 0; n < 32; ++n)
        __asm__ volatile("nop");
    REG32(GLB) &= ~(0xffu << 8);         /* HCLK divider = 0 */
    /* SDK clock-divider latch sequence; all code/data here are in TCM. */
    REG32(GLB + 0xffc) = 1;
    REG32(GLB + 0xffc) = 0;
    REG32(GLB + 0x24) |= 1u << 16;       /* UART0 bus gate */
    REG32(GLB + 0x08) &= ~0x17u;         /* UART clock disabled, divider = 0 */
    REG32(GLB + 0x08) |= 1u << 4;
    /* GPIO14/15 UART_SIG6/7 (swap disabled), TX0=2, RX0=3. */
    REG32(GLB + 0x80) &= ~(0xfu << 24);
    REG32(GLB + 0xc0) = (REG32(GLB + 0xc0) & 0x00ffffffu) | 0x32000000u;
    REG32(GLB + 0x11c) = 0x07110711u;    /* func 7, pullup, input enable */
    REG32(GLB + 0x190) &= ~((1u << 14) | (1u << 15));
    REG32(UART) = 0;
    REG32(UART + 4) = 0;
    uint32_t period = (32000000u + UART_BAUD / 2) / UART_BAUD - 1u;
    REG32(UART + 8) = period | (period << 16);
    REG32(UART + 0x0c) = 0;             /* LSB first, no bit inversion */
    REG32(UART + 0x1c) = 0;             /* default RTS software control */
    REG32(UART + 0x24) = 0xffffffffu;    /* mask all UART interrupts */
    REG32(UART + 0x80) = 0x0cu;         /* clear FIFOs, DMA off */
    REG32(UART + 0x80) = 0;
    REG32(UART) = 1u | (1u << 2) | (7u << 8) | (1u << 11); /* TX free-run, 8N1 */
    REG32(UART + 4) = 1u | (7u << 8);  /* RX, 8 data, no parity */
}

static void putc_uart(char c) {
    while (!(REG32(UART + 0x84) & 0xffu)) {}
    REG32(UART + 0x88) = (unsigned char)c;
}
static void puts_uart(const char *s) {
    while (*s) {
        if (*s == '\n') putc_uart('\r');
        putc_uart(*s++);
    }
}
static void hex(uint32_t value) {
    for (int i = 28; i >= 0; i -= 4)
        putc_uart("0123456789abcdef"[(value >> i) & 15u]);
}
static void info(void) {
    uint32_t value;
    __asm__ volatile("csrr %0, misa" : "=r"(value));
    puts_uart("MISA=0x"); hex(value);
    __asm__ volatile("csrr %0, mhartid" : "=r"(value));
    puts_uart(" HART=0x"); hex(value);
    puts_uart("\n");
}
static int ram_check(void) {
    for (unsigned i = 0; i < 32; ++i) bss_check[i] = 0xa20b0000u ^ i;
    for (unsigned i = 0; i < 32; ++i)
        if (bss_check[i] != (0xa20b0000u ^ i)) return 0;
    return 1;
}
static void led_init(void) {
    /* M0Sense RGB is active low on GPIO23/24/25 (Sipeed io_def.h).
     * BL702 uses alternate pad config registers when PSRAM-IO is selected;
     * match the vendor GPIO driver's real_pin translation. */
    for (unsigned pin = 23; pin <= 25; ++pin) {
        unsigned real_pin = pin;
        if (REG32(GLB + 0x88) & (1u << (pin - 23))) real_pin += 9;
        unsigned shift = (real_pin & 1u) * 16;
        uintptr_t cfg = GLB + 0x100 + (real_pin / 2) * 4;
        REG32(cfg) = (REG32(cfg) & ~(0xffffu << shift)) | ((11u << 8) << shift);
    }
    REG32(GLB + 0x188) |= 7u << 23;      /* all LEDs off */
    REG32(GLB + 0x190) |= 7u << 23;      /* GPIO outputs */
}
void probe_fatal(uint32_t cause, uint32_t pc, uint32_t value) {
    probe_stage = 0xdead0000u | (cause & 0xffffu);
    puts_uart("A20 BL702 TRAP cause="); hex(cause);
    puts_uart(" pc="); hex(pc); puts_uart(" value="); hex(value); puts_uart("\n");
    for (;;) {}
}
void probe_main(void) {
    probe_stage = 1;
    uart_init();
    puts_uart("A20 BL702 M-mode SRAM probe\n");
    info();
    for (unsigned i = 0; i < 32; ++i)
        if (bss_check[i]) probe_fatal(0xff, 0, i);
    if (data_check != 0x41323050u || !ram_check()) probe_fatal(0xfe, 0, data_check);
    led_init();
    probe_stage = 2;
    puts_uart("A20 BL702 PROBE READY\nCommands: i=info m=RAM check, other bytes echo\n");
    unsigned heartbeat = 0;
    for (;;) {
        if (++heartbeat == 500000u) {
            REG32(GLB + 0x188) ^= 1u << 23;
            heartbeat = 0;
        }
        if ((REG32(UART + 0x84) >> 8) & 0xffu) {
            char c = (char)REG32(UART + 0x8c);
            if (c == 'i') info();
            else if (c == 'm') puts_uart(ram_check() ? "RAM PASS\n" : "RAM FAIL\n");
            else putc_uart(c);
        }
    }
}
