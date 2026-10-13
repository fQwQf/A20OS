#ifdef CONFIG_QEMU_SHARED_SMP
#include "core/shared_device_call.h"
#include "core/errno.h"
#include "cpu.h"

enum { CONSOLE_PUTC, CONSOLE_GETC, CONSOLE_FLUSH };
static int console_operation(uint32_t operation, uint64_t argument, uint64_t *result) {
    /* Only owner CPU 0 touches its physical UART; each transaction is a leaf
     * so service can run even while owner spins on a lock held by its caller. */
    switch (operation) {
    case CONSOLE_PUTC:
        if (!(inb(0x3fd) & 0x20)) return -EAGAIN;
        outb(0x3f8, (uint8_t)argument);
        *result = 0;
        return 0;
    case CONSOLE_GETC:
        *result = (inb(0x3fd) & 1) ? inb(0x3f8) : (~0ULL);
        return 0;
    case CONSOLE_FLUSH:
        *result = 0;
        return (inb(0x3fd) & 0x40) ? 0 : -EAGAIN;
    default:
        return -EINVAL;
    }
}
static uint64_t console_call(uint32_t operation, uint64_t argument) {
    uint64_t result = (~0ULL);
    int status;
    do {
        status = shared_device_call(0, console_operation, operation, argument, &result);
        if (status == -EAGAIN) arch_cpu_relax();
    } while (status == -EAGAIN);
    /* No console recursion on an invalid context; the caller observes no RX
     * byte, while normal boot/IRQ/task callers have a valid owner endpoint. */
    return status ? (~0ULL) : result;
}
void x86_shared_console_putc(char c) { (void)console_call(CONSOLE_PUTC, (uint8_t)c); }
int x86_shared_console_getc(void) {
    uint64_t value = console_call(CONSOLE_GETC, 0);
    return value <= 255 ? (int)value : -1;
}
void x86_shared_console_flush(void) { (void)console_call(CONSOLE_FLUSH, 0); }
#endif
