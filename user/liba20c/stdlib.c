#include <stdlib.h>

int atoi(const char *str)
{
    const char *s = str;

    while (*s == ' '  || *s == '\t' || *s == '\n' ||
           *s == '\r' || *s == '\f' || *s == '\v') {
        s++;
    }

    int sign = 1;
    if (*s == '-') {
        sign = -1;
        s++;
    } else if (*s == '+') {
        s++;
    }

    long val = 0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }

    return sign * (int)val;
}
