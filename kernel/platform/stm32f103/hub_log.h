#ifndef _STM32F103_HUB_LOG_H
#define _STM32F103_HUB_LOG_H

/*
 * Log-line formatter for the TF-card run log (/LOG/RUN.LOG). Pure string logic
 * (host-testable); the actual append is done via sdfs. Timestamps are the
 * uptime in milliseconds, rendered as HHH:MM:SS.mmm (swap in RTC wall-clock
 * later without changing the format).
 *
 *   012:34:56.789 [CTRL] fan=2 pump=0
 */

#include "core/types.h"

/*
 * Format one log line (with a trailing '\n') into buf. ts_ms is milliseconds
 * since boot; tag is a short category (e.g. "CTRL", "NET"); msg is the body.
 * Returns the line length written (excluding the NUL), or -1 on bad args /
 * insufficient buffer. Never overflows buf.
 */
int hub_log_format(char *buf, unsigned cap, uint32_t ts_ms, const char *tag,
                   const char *msg);

#endif /* _STM32F103_HUB_LOG_H */
