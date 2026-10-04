/* Freestanding shim: direct declarations for the C string functions
 * littlefs uses, satisfied by the kernel's core/string.c at link time.
 * Deliberately includes no kernel headers so the vendored sources compile
 * against nothing but these declarations. */
#ifndef LFS_COMPAT_STRING_H
#define LFS_COMPAT_STRING_H

typedef __SIZE_TYPE__ size_t;

void *memcpy(void *dst, const void *src, unsigned long n);
void *memset(void *dst, int c, unsigned long n);
void *memmove(void *dst, const void *src, unsigned long n);
int   memcmp(const void *a, const void *b, unsigned long n);
unsigned long strlen(const char *s);
int   strcmp(const char *a, const char *b);
int   strncmp(const char *a, const char *b, unsigned long n);
char *strncpy(char *dst, const char *src, unsigned long n);
char *strchr(const char *s, int c);
char *strcpy(char *dst, const char *src);
char *strcat(char *dst, const char *src);
char *strrchr(const char *s, int c);
int   strcoll(const char *a, const char *b);
unsigned long strcspn(const char *s, const char *reject);
unsigned long strspn(const char *s, const char *accept);
char *strstr(const char *h, const char *n);

#ifndef NULL
#define NULL ((void *)0)
#endif

#endif
