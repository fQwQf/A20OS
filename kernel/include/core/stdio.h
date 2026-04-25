#ifndef _STDIO_H
#define _STDIO_H

#include "core/types.h"

/* Variadic support without stdarg.h */
typedef __builtin_va_list va_list;
#define va_start(ap, param) __builtin_va_start(ap, param)
#define va_end(ap)          __builtin_va_end(ap)
#define va_arg(ap, type)    __builtin_va_arg(ap, type)

/* Output */
int puts(const char *s);
int putchar(char c);
void printf(const char *fmt, ...);
void vprintf(const char *fmt, va_list args);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list args);

/* Input */
int getchar(void);

/* Debug */
#ifdef DEBUG
#define debug(...) printf("[DEBUG] " __VA_ARGS__)
#else
#define debug(...) do {} while(0)
#endif

#endif /* _STDIO_H */
