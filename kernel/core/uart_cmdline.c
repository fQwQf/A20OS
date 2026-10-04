/*
 * A20OS — kernel command line typed on the console
 *
 * A bring-up facility, compiled in only when CONFIG_UART_CMDLINE=y (the
 * default is off).  It exists for one situation: firmware that hands the
 * kernel no command line at all, so there is no way to configure any `a20.*`
 * key -- no a20.wx, no a20.tcpmode, no a20.xlator.  LoongArch64 under QEMU's
 * virt machine is exactly that, and QEMU offers no other route (see
 * docs/exec-xlator/01-usage.md); on a board whose UEFI does populate
 * /chosen/bootargs this is redundant and stays off.
 *
 * WHY THIS IS OFF BY DEFAULT, and not just "for performance"
 * ---------------------------------------------------------
 * The command line is the one piece of kernel configuration an operator
 * cannot change without rebuilding the image: it is fixed by whoever built
 * the boot medium.  Reading it from a serial port moves that authority to
 * whoever is holding the serial cable at boot.  That is a real privilege --
 * it includes choosing the a20.xlator.<guest> path, and therefore choosing
 * which binary the kernel will re-exec a foreign image into.  An image
 * booted unattended has no such exposure; an image booted by an operator who
 * owns the console does.
 *
 * So it is gated at compile time rather than at runtime.  A boot key cannot
 * enable it: reading the boot key is what this facility is for, so there is
 * nothing yet to read one from.  That forces the decision to build-time,
 * which is the right place for it -- an unattended build should not be able
 * to acquire a console input path at all.
 *
 * The reader itself is arch-independent; arch_bootargs_get() decides when to
 * call it.  Keep the call sites in the arch hooks rather than in
 * core/bootargs.c, so an architecture with a working firmware command line
 * never prompts even when the option is on.
 */

#include "core/bootargs.h"

#ifdef CONFIG_UART_CMDLINE

#include "core/string.h"
#include "core/timer.h"
#include "drivers/char/uart.h"
#include "platform.h"

/* Matches BOOTARGS_MAX in core/bootargs.c, which is where the result lands
 * and the real ceiling on what survives. */
#define UART_CMDLINE_MAX 1024

/* Inactivity window, not a total budget: the operator may take as long as
 * they like as long as they keep typing.  A line that is never finished
 * still ends the wait, so an unattended console cannot stall the boot
 * forever -- it just boots with no command line, as it did before.
 *
 * The deadline is measured on timer_get_ticks(), which on LoongArch64 is the
 * rdtime.d counter and runs whether or not the periodic timer is enabled --
 * timer_enable() happens much later in main.c.  The ARCH_TIMER_FREQ idiom and
 * the 5-second window are copied from the QEMU riscv64 board's existing
 * console wait.
 */
#define UART_CMDLINE_TIMEOUT_S 5

int uart_cmdline_read(char *out, size_t outsz)
{
    if (!out || outsz == 0)
        return -1;
    out[0] = '\0';

    size_t max = outsz - 1;
    if (max > UART_CMDLINE_MAX - 1)
        max = UART_CMDLINE_MAX - 1;

    uart_puts("[UARTCMD] type a kernel command line, or press Enter for "
              "none\n[UARTCMD] ");

    const uint64_t window = UART_CMDLINE_TIMEOUT_S * ARCH_TIMER_FREQ;
    uint64_t wait_start = timer_get_ticks();
    size_t len = 0;

    for (;;) {
        /* Unsigned subtraction: monotonic, so this reads as elapsed time and
         * stays correct across the counter's own wrap.  Same idiom as the
         * console wait in kernel/platform/qemu-virt-riscv64/board.c. */
        if (timer_get_ticks() - wait_start > window)
            break;

        int c = uart_try_getc();
        if (c < 0)
            continue;

        if (c == '\r' || c == '\n') {
            uart_puts("\n");
            break;
        }
        if (c == '\b' || c == 0x7f) {
            if (len) {
                len--;
                uart_puts("\b \b");
            }
        } else if (len < max) {
            out[len++] = (char)c;
            uart_putc((char)c);
        }
        /* Past the cap the characters are dropped rather than the line
         * truncated mid-token: bootargs_init() bounds it again, and a
         * silently shortened `a20.xlator.x86_64` would look configured while
         * pointing somewhere else. */
        wait_start = timer_get_ticks();
    }

    out[len] = '\0';
    if (len == 0) {
        uart_puts("[UARTCMD] no command line given, booting without one\n");
        return -1;
    }

    uart_puts("[UARTCMD] using command line from the console\n");
    return (int)len;
}

#endif /* CONFIG_UART_CMDLINE */