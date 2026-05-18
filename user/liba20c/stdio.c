/*
 * A20OS liba20c — stdio implementation.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include "fdtable.h"
#include "../liba20rt/a20_syscall.h"

#define BUFSZ 4096

struct _IO_FILE {
    uint32_t handle;
    int      fd;
    uint8_t  mode;
    uint8_t  own_buf;
    char    *buf;
    size_t   buf_pos;
    size_t   buf_len;
    size_t   buf_cap;
};

static FILE _stdin_file;
static FILE _stdout_file;
static FILE _stderr_file;

FILE *stdin  = &_stdin_file;
FILE *stdout = &_stdout_file;
FILE *stderr = &_stderr_file;

static void _file_init(FILE *f, uint32_t handle, int fd, int mode)
{
    f->handle  = handle;
    f->fd      = fd;
    f->mode    = (uint8_t)mode;
    f->own_buf = 0;
    f->buf     = NULL;
    f->buf_pos = 0;
    f->buf_len = 0;
    f->buf_cap = 0;
}

void __stdio_init(uint32_t h_stdin, uint32_t h_stdout, uint32_t h_stderr)
{
    _file_init(&_stdin_file,  h_stdin,  0, 0);
    _file_init(&_stdout_file, h_stdout, 1, 1);
    _file_init(&_stderr_file, h_stderr, 2, 1);
}

static int _fflush_locked(FILE *f)
{
    if (f->buf_pos > 0 && f->buf) {
        uint64_t args[4] = {f->handle, (uint64_t)(uintptr_t)f->buf, f->buf_pos, 0};
        a20_handle_write(args);
        f->buf_pos = 0;
        f->buf_len = 0;
    }
    return 0;
}

int fflush(FILE *f)
{
    if (f) return _fflush_locked(f);
    _fflush_locked(stdout);
    _fflush_locked(stderr);
    return 0;
}

FILE *fopen(const char *path, const char *mode)
{
    uint32_t rights = 0;
    if (mode[0] == 'r') rights = 1;
    else if (mode[0] == 'w') rights = 2 | 1;
    else if (mode[0] == 'a') rights = 2 | 1;

    uint64_t open_args[6] = {0, (uint64_t)(uintptr_t)path, (uint64_t)strlen(path),
                             rights, 0, 0};
    int64_t h = a20_path_open(open_args);
    if ((int64_t)h < 0) return NULL;

    int fd = __fd_alloc((uint32_t)h);
    if (fd < 0) { a20_handle_close((uint32_t)h); return NULL; }

    FILE *f = (FILE *)malloc(sizeof(FILE));
    if (!f) { __fd_free(fd); a20_handle_close((uint32_t)h); return NULL; }

    _file_init(f, (uint32_t)h, fd, mode[0] == 'r' ? 0 : 1);
    f->own_buf = 1;
    f->buf = (char *)malloc(BUFSZ);
    f->buf_cap = BUFSZ;
    return f;
}

int fclose(FILE *f)
{
    if (!f) return 0;
    _fflush_locked(f);
    a20_handle_close(f->handle);
    __fd_free(f->fd);
    if (f->own_buf && f->buf) free(f->buf);
    if (f != &_stdin_file && f != &_stdout_file && f != &_stderr_file)
        free(f);
    return 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f)
{
    if (!f || !ptr || size == 0) return 0;
    size_t total = size * nmemb;
    uint64_t args[4] = {f->handle, (uint64_t)(uintptr_t)ptr, total, 0};
    int64_t r = a20_handle_read(args);
    if (r < 0) return 0;
    return (size_t)r / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f)
{
    if (!f || !ptr || size == 0) return 0;
    size_t total = size * nmemb;
    uint64_t args[4] = {f->handle, (uint64_t)(uintptr_t)ptr, total, 0};
    int64_t r = a20_handle_write(args);
    if (r < 0) return 0;
    return (size_t)r / size;
}

int fputc(int c, FILE *f)
{
    char ch = (char)c;
    return fwrite(&ch, 1, 1, f) == 1 ? c : -1;
}

int fputs(const char *s, FILE *f)
{
    size_t len = strlen(s);
    return fwrite(s, 1, len, f) == len ? 0 : -1;
}

int puts(const char *s)
{
    int r = fputs(s, stdout);
    fputc('\n', stdout);
    return r;
}

int fgetc(FILE *f)
{
    char c;
    if (fread(&c, 1, 1, f) != 1) return -1;
    return (unsigned char)c;
}

char *fgets(char *s, int size, FILE *f)
{
    int i = 0;
    while (i < size - 1) {
        int c = fgetc(f);
        if (c == -1) break;
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return NULL;
    s[i] = '\0';
    return s;
}

long fseek(FILE *f, long offset, int whence)
{
    _fflush_locked(f);
    int64_t r = a20_handle_seek(f->handle, (uint64_t)offset, (uint64_t)whence);
    return r < 0 ? -1 : 0;
}
