#ifndef _PANIC_H
#define _PANIC_H

#include "core/types.h"
#include "core/defs.h"

NORETURN void panic(const char *fmt, ...);

#endif /* _PANIC_H */
