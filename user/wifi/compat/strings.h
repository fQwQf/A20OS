#ifndef A20_RTW_STRINGS_H
#define A20_RTW_STRINGS_H
#include <stddef.h>
int ffs(unsigned int);
int strcasecmp(const char *, const char *);
int strncasecmp(const char *, const char *, size_t);
void bzero(void *, size_t);
void bcopy(const void *, void *, size_t);
#endif
