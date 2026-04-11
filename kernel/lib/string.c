#include "string.h"
#include "mm.h"

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && *s1 == *s2) { s1++; s2++; }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && *s1 == *s2) { s1++; s2++; n--; }
    if (!n) return 0;
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)); return r; }

char *strncpy(char *d, const char *s, size_t n) {
    size_t i;
    for (i = 0; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = '\0';
    return d;
}

char *strcat(char *d, const char *s) { char *r = d; while (*d) d++; while ((*d++ = *s++)); return r; }

char *strchr(const char *s, int c) { while (*s) { if (*s == (char)c) return (char*)s; s++; } return NULL; }

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    while (*s) { if (*s == (char)c) last = s; s++; }
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0) return (char *)haystack;
    while (*haystack) {
        if (*haystack == *needle && strncmp(haystack, needle, nlen) == 0)
            return (char *)haystack;
        haystack++;
    }
    return NULL;
}

static char *strtok_save = NULL;
char *strtok(char *str, const char *delim) {
    if (!str) str = strtok_save;
    if (!str) return NULL;
    while (*str && strchr(delim, *str)) str++;
    if (!*str) { strtok_save = NULL; return NULL; }
    char *tok = str;
    while (*str && !strchr(delim, *str)) str++;
    if (*str) { *str = '\0'; strtok_save = str + 1; }
    else strtok_save = NULL;
    return tok;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const unsigned char *a = s1, *b = s2;
    while (n--) { if (*a != *b) return *a - *b; a++; b++; }
    return 0;
}

void *memset(void *s, int c, size_t n) {
    unsigned char *p = s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *dp = d; const unsigned char *sp = s;
    while (n--) *dp++ = *sp++;
    return d;
}

void *memmove(void *d, const void *s, size_t n) {
    unsigned char *dp = d; const unsigned char *sp = s;
    if (dp < sp) { while (n--) *dp++ = *sp++; }
    else { dp += n; sp += n; while (n--) *--dp = *--sp; }
    return d;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    while (n--) { if (*p == (unsigned char)c) return (void*)p; p++; }
    return NULL;
}

int atoi(const char *s) {
    int r = 0, sign = 1;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') r = r * 10 + (*s++ - '0');
    return sign * r;
}

long atol(const char *s) {
    long r = 0; int sign = 1;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') r = r * 10 + (*s++ - '0');
    return sign * r;
}

int strcasecmp(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        int c1 = *s1, c2 = *s2;
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return c1 - c2;
        s1++; s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

char *strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *d = kmalloc(len);
    if (d) memcpy(d, s, len);
    return d;
}
