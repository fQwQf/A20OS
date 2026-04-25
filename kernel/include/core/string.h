#ifndef _STRING_H
#define _STRING_H

#include "core/types.h"

size_t  strlen(const char *s);
int     strcmp(const char *s1, const char *s2);
int     strncmp(const char *s1, const char *s2, size_t n);
char   *strcpy(char *dest, const char *src);
char   *strncpy(char *dest, const char *src, size_t n);
char   *strcat(char *dest, const char *src);
char   *strchr(const char *s, int c);
char   *strrchr(const char *s, int c);
char   *strstr(const char *haystack, const char *needle);
char   *strtok(char *str, const char *delim);
int     memcmp(const void *s1, const void *s2, size_t n);
void   *memset(void *s, int c, size_t n);
void   *memcpy(void *dest, const void *src, size_t n);
void   *memmove(void *dest, const void *src, size_t n);
void   *memchr(const void *s, int c, size_t n);
int     atoi(const char *s);
long    atol(const char *s);
int     strcasecmp(const char *s1, const char *s2);
char   *strdup(const char *s);

#endif /* _STRING_H */
