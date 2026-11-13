#include "platform.h"
#include "console.h"

int arch_uart_irq(void) {
    return x86_uart_present() ? IRQ_VECTOR_UART : -1;
}

/*
 * Cached COM1 presence probe.  The presence test is the 16550 scratch
 * register: write a pattern to offset 7 and read it back.  Firmware on a
 * machine with no SuperIO COM port leaves the legacy window undecoded, so the
 * read returns 0xFF.  Caching matters because the inline console path asks on
 * every emitted chunk, and the probe itself performs port I/O.
 *
 * A false negative would only lose the serial console -- the UEFI framebuffer
 * console (drivers/char/fbcon.c) is already active by then -- whereas a false
 * positive would bring back the boot hang this exists to prevent.  The test is
 * therefore the standard one and nothing weaker.
 */
int x86_uart_present(void) {
    static int present = -1;
    if (present >= 0)
        return present;
    outb(0x3F8 + 7, 0xAE);
    present = inb(0x3F8 + 7) == 0xAE;
    if (present) {
        /* Loopback sanity: a decoded scratch register alone can belong to a
         * different LPC device.  A 16550 in loopback returns the byte written
         * to the transmit register from the receive register. */
        outb(0x3F8 + 4, 0x1E);  /* MCR: loopback, OUT2, RTS, DTR */
        outb(0x3F8 + 0, 0x5A);
        for (unsigned i = 0; i < 100000 && !(inb(0x3F8 + 5) & 0x01); i++)
            ;
        present = inb(0x3F8 + 5) & 0x01 && inb(0x3F8 + 0) == 0x5A;
        outb(0x3F8 + 4, 0x0F);  /* leave loopback */
    }
    return present;
}
