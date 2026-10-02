#ifndef A20_LWIP_PORT_STDLIB_H
#define A20_LWIP_PORT_STDLIB_H

#include "core/types.h"
#include "core/string.h"

long strtol(const char *nptr, char **endptr, int base);
int atoi(const char *nptr);

#endif /* A20_LWIP_PORT_STDLIB_H */
