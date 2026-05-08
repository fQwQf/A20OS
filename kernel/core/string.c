#include "core/string.h"
#include "mm/mm.h"

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

char *strtok_r(char *str, const char *delim, char **saveptr) {
    if (!saveptr) return NULL;
    if (!str) str = *saveptr;
    if (!str) return NULL;
    while (*str && strchr(delim, *str)) str++;
    if (!*str) { *saveptr = NULL; return NULL; }
    char *tok = str;
    while (*str && !strchr(delim, *str)) str++;
    if (*str) { *str = '\0'; *saveptr = str + 1; }
    else *saveptr = NULL;
    return tok;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const unsigned char *a = s1, *b = s2;
    while (n--) { if (*a != *b) return *a - *b; a++; b++; }
    return 0;
}

void *memset(void *s, int c, size_t n) {
    unsigned char *p = s;
    unsigned char v = (unsigned char)c;
    /* Word-fill the bulk (8 bytes at a time) */
    while (n >= 8 && ((uintptr_t)p & 7)) {
        *p++ = v;
        n--;
    }
    if (n >= 8) {
        uint64_t w = v;
        w |= w << 8;  w |= w << 16; w |= w << 32;
        uint64_t *wp = (uint64_t *)p;
        while (n >= 64) {
            wp[0] = w; wp[1] = w; wp[2] = w; wp[3] = w;
            wp[4] = w; wp[5] = w; wp[6] = w; wp[7] = w;
            wp += 8; n -= 64;
        }
        while (n >= 8) { *wp++ = w; n -= 8; }
        p = (unsigned char *)wp;
    }
    while (n--) *p++ = v;
    return s;
}

void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *dp = d; const unsigned char *sp = s;
    if (n >= 64 && !((uintptr_t)dp & 7) && !((uintptr_t)sp & 7)) {
        uint64_t *dw = (uint64_t *)dp; const uint64_t *sw = (const uint64_t *)sp;
        while (n >= 64) {
            dw[0] = sw[0]; dw[1] = sw[1]; dw[2] = sw[2]; dw[3] = sw[3];
            dw[4] = sw[4]; dw[5] = sw[5]; dw[6] = sw[6]; dw[7] = sw[7];
            dw += 8; sw += 8; n -= 64;
        }
        while (n >= 8) { *dw++ = *sw++; n -= 8; }
        dp = (unsigned char *)dw; sp = (const unsigned char *)sw;
    } else {
        while (n >= 8 && !((uintptr_t)dp & 7) && !((uintptr_t)sp & 7)) {
            *(uint64_t *)dp = *(const uint64_t *)sp;
            dp += 8; sp += 8; n -= 8;
        }
    }
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
