#include "core/types.h"

long strtol(const char *nptr, char **endptr, int base) {
    const char *p = nptr;
    long sign = 1;
    long value = 0;

    while (*p == ' ' || *p == '\t' || *p == '\n' ||
           *p == '\r' || *p == '\f' || *p == '\v')
        p++;
    if (*p == '-') {
        sign = -1;
        p++;
    } else if (*p == '+') {
        p++;
    }

    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    } else if (base == 0 && p[0] == '0') {
        base = 8;
        p++;
    } else if (base == 0) {
        base = 10;
    }

    while (*p) {
        int digit;
        if (*p >= '0' && *p <= '9')
            digit = *p - '0';
        else if (*p >= 'a' && *p <= 'z')
            digit = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'Z')
            digit = *p - 'A' + 10;
        else
            break;
        if (digit >= base)
            break;
        value = value * base + digit;
        p++;
    }

    if (endptr)
        *endptr = (char *)p;
    return value * sign;
}


#include "core/lock.h"
#include "arch/cc.h"

sys_prot_t sys_arch_protect(void) {
    uint64_t flags = arch_irqs_enabled() ? 1 : 0;
    arch_local_irq_disable();
    return flags;
}

void sys_arch_unprotect(sys_prot_t pval) {
    if (pval)
        arch_local_irq_enable();
}
