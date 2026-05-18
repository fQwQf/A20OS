#ifndef _STRING_H
#define _STRING_H

#include <stddef.h>

void   *memcpy(void *dest, const void *src, size_t n);
void   *memset(void *s, int c, size_t n);
void   *memmove(void *dest, const void *src, size_t n);
int     memcmp(const void *s1, const void *s2, size_t n);
void   *memchr(const void *s, int c, size_t n);

size_t  strlen(const char *s);
int     strcmp(const char *a, const char *b);
int     strncmp(const char *a, const char *b, size_t n);
char   *strcpy(char *dest, const char *src);
char   *strncpy(char *dest, const char *src, size_t n);
char   *strcat(char *dest, const char *src);
char   *strchr(const char *s, int c);
char   *strrchr(const char *s, int c);

#endif
