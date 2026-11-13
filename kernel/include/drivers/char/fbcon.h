#ifndef A20_DRIVERS_CHAR_FBCON_H
#define A20_DRIVERS_CHAR_FBCON_H

#include "core/types.h"

/*
 * Linear-scanout text console.
 *
 * The kernel's only console sink used to be the 16550 UART, so a machine with
 * no COM1 (every laptop) booted invisibly.  fbcon renders the same byte stream
 * the UART receives onto the firmware-preserved GOP scanout, which is what the
 * x86_64 UEFI loader hands over.  It is a text renderer, not a graphics stack:
 * no modesetting, no GPU commands, no userspace involvement.
 *
 * fbcon_init() is a no-op when the boot handoff carries no framebuffer or when
 * the architecture has no scanout mapping hook.  Callers therefore never need
 * to know whether a display is present.
 */
void fbcon_init(void);
/* Attach after the page allocator is ready and replay the kernel log that was
 * produced while the scanout could not yet be mapped (a GOP framebuffer above
 * 4 GiB needs a dynamic mapping).  No-op when fbcon is already active. */
void fbcon_attach_late(void);
int fbcon_active(void);
void fbcon_putc(char c);
void fbcon_write(const char *data, size_t length);
/* Mirror console output before the scanout is mappable.  uart_write() calls
 * this unconditionally; it is a no-op once fbcon is active. */
void fbcon_capture(const char *data, size_t length);
/* Hand the scanout to a compositor; restore after its exit or for panic. */
void fbcon_suspend(void);
void fbcon_resume(void);

#endif /* A20_DRIVERS_CHAR_FBCON_H */
