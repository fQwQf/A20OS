#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>

typedef struct _IO_FILE FILE;

#define EOF     (-1)
#define BUFSIZ  512

#define _IOFBF  0
#define _IOLBF  1
#define _IONBF  2

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE  *fopen(const char *path, const char *mode);
int    fclose(FILE *f);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f);
int    fflush(FILE *f);
int    fputc(int c, FILE *f);
int    fputs(const char *s, FILE *f);
int    puts(const char *s);
int    fgetc(FILE *f);
char  *fgets(char *s, int size, FILE *f);
long   fseek(FILE *f, long offset, int whence);

int    setvbuf(FILE *stream, char *buf, int mode, size_t size);

int    feof(FILE *stream);
int    ferror(FILE *stream);
void   clearerr(FILE *stream);
void   perror(const char *s);

int    printf(const char *fmt, ...);
int    fprintf(FILE *f, const char *fmt, ...);
int    sprintf(char *buf, const char *fmt, ...);
int    snprintf(char *buf, size_t size, const char *fmt, ...);
int    vsnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list ap);

#define getchar()  fgetc(stdin)
#define putchar(c) fputc((c), stdout)

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#endif
