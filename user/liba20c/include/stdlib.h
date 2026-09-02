#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

#define ATEXIT_MAX 8

void  *malloc(size_t size);
void   free(void *ptr);
void  *calloc(size_t nmemb, size_t size);
void  *realloc(void *ptr, size_t size);
void   exit(int code);
int    atoi(const char *s);
int    atexit(void (*func)(void));

#endif
