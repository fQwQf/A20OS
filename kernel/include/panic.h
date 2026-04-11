#ifndef _PANIC_H
#define _PANIC_H

#include "types.h"
#include "defs.h"

NORETURN void panic(const char *fmt, ...);

#endif /* _PANIC_H */
