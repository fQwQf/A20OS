/*
 * A20OS Native SDK — Pure string / memory utilities.
 *
 * No syscall dependency. Freestanding C library subset.
 */
#ifndef _A20_STRING_H
#define _A20_STRING_H

#include <stdint.h>
#include <stddef.h>

static inline void *a20_memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

static inline void *a20_memset(void *dst, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

static inline void *a20_memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

static inline int a20_memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    while (n--) {
        if (*pa != *pb) return *pa - *pb;
        pa++; pb++;
    }
    return 0;
}

static inline size_t a20_strlen(const char *s)
{
    size_t len = 0;
    while (*s++) len++;
    return len;
}

static inline int a20_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *(const uint8_t *)a - *(const uint8_t *)b;
}

static inline int a20_strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (n == 0) return 0;
    return *(const uint8_t *)a - *(const uint8_t *)b;
}

static inline char *a20_strchr(const char *s, int c)
{
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == 0) ? (char *)s : (void *)0;
}

static inline char *a20_strcpy(char *dst, const char *src)
{
    char *ret = dst;
    while ((*dst++ = *src++));
    return ret;
}

static inline char *a20_strncpy(char *dst, const char *src, size_t n)
{
    char *ret = dst;
    while (n && (*dst++ = *src++)) n--;
    while (n--) *dst++ = '\0';
    return ret;
}

static inline void *a20_memchr(const void *s, int c, size_t n)
{
    const uint8_t *p = (const uint8_t *)s;
    while (n--) {
        if (*p == (uint8_t)c) return (void *)p;
        p++;
    }
    return (void *)0;
}

#endif
