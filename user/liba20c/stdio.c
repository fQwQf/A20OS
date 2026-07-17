/*
 * A20OS liba20c — stdio implementation with buffered I/O.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <errno.h>
#include "fdtable.h"
#include "../liba20rt/a20_syscall.h"
#include "../liba20rt/crt0_a20.h"

struct _IO_FILE {
    uint32_t handle;
    int      fd;
    uint8_t  mode;       /* 0 = read, 1 = write */
    uint8_t  buf_mode;   /* _IOFBF / _IOLBF / _IONBF */
    uint8_t  own_buf;
    uint8_t  eof;
    uint8_t  err;
    char    *buf;
    size_t   buf_pos;    /* read cursor or write fill pointer */
    size_t   buf_len;    /* valid bytes in buffer */
    size_t   buf_cap;    /* buffer capacity */
};

static FILE _stdin_file;
static FILE _stdout_file;
static FILE _stderr_file;

static char _stdin_buf[BUFSIZ];
static char _stdout_buf[BUFSIZ];

FILE *stdin  = &_stdin_file;
FILE *stdout = &_stdout_file;
FILE *stderr = &_stderr_file;

static void _file_init(FILE *f, uint32_t handle, int fd, int mode)
{
    f->handle  = handle;
    f->fd      = fd;
    f->mode    = (uint8_t)mode;
    f->buf_mode = _IONBF;
    f->own_buf = 0;
    f->buf     = NULL;
    f->buf_pos = 0;
    f->buf_len = 0;
    f->buf_cap = 0;
    f->eof     = 0;
    f->err    = 0;
}

void __stdio_init(uint32_t h_stdin, uint32_t h_stdout, uint32_t h_stderr)
{
    _file_init(&_stdin_file,  h_stdin,  0, 0);
    _stdin_file.buf_mode = _IOFBF;
    _stdin_file.buf      = _stdin_buf;
    _stdin_file.buf_cap  = BUFSIZ;

    _file_init(&_stdout_file, h_stdout, 1, 1);
    _stdout_file.buf_mode = _IOLBF;
    _stdout_file.buf      = _stdout_buf;
    _stdout_file.buf_cap  = BUFSIZ;

    _file_init(&_stderr_file, h_stderr, 2, 1);
    _stderr_file.buf_mode = _IONBF;
}

void __liba20c_init(void)
{
    __fd_table_init();
    a20_start_info_t *si = a20_get_start_info();
    if (si) {
        __stdio_init(si->stdin_handle, si->stdout_handle, si->stderr_handle);
    } else {
        __stdio_init(A20_HANDLE_NULL, A20_HANDLE_NULL, A20_HANDLE_NULL);
    }
}

static int _write_all(FILE *f, const char *ptr, size_t len)
{
    if (len == 0) return 0;

    a20_iovec_t iov;
    a20_io_args_t args;
    size_t done = 0;

    while (done < len) {
        iov.base = (uint64_t)(uintptr_t)(ptr + done);
        iov.len  = len - done;

        args.size       = sizeof(args);
        args.version    = 1;
        args.handle     = f->handle;
        args._pad0      = 0;
        args.iov        = (uint64_t)&iov;
        args.iov_count  = 1;
        args._pad1      = 0;
        args.offset     = A20_OFFSET_CURRENT;
        args.out_count  = 0;

        int64_t r = a20_syscall6(A20_SYS_handle_write, (uint64_t)&args, 0, 0, 0, 0, 0);
        if (r < 0) {
            f->err = 1;
            return -1;
        }
        if (args.out_count == 0) {
            f->err = 1;
            return -1;
        }
        done += (size_t)args.out_count;
    }
    return 0;
}

static int _fflush_locked(FILE *f)
{
    if (!f) return -1;
    if (f->mode == 1 && f->buf_len > 0) {
        if (_write_all(f, f->buf, f->buf_len) < 0)
            return -1;
    }
    f->buf_pos = 0;
    f->buf_len = 0;
    return 0;
}

int fflush(FILE *f)
{
    if (f) return _fflush_locked(f);
    _fflush_locked(stdout);
    _fflush_locked(stderr);
    return 0;
}

int setvbuf(FILE *stream, char *buf, int mode, size_t size)
{
    if (!stream) return -1;
    if (mode != _IONBF && mode != _IOLBF && mode != _IOFBF) return -1;

    _fflush_locked(stream);

    if (stream->own_buf && stream->buf) {
        free(stream->buf);
        stream->own_buf = 0;
    }

    stream->buf_mode = (uint8_t)mode;
    stream->buf_pos = 0;
    stream->buf_len = 0;

    if (mode == _IONBF) {
        stream->buf = NULL;
        stream->buf_cap = 0;
        return 0;
    }

    if (buf && size > 0) {
        stream->buf = buf;
        stream->buf_cap = size;
    } else if (stream == stdin) {
        stream->buf = _stdin_buf;
        stream->buf_cap = BUFSIZ;
    } else if (stream == stdout) {
        stream->buf = _stdout_buf;
        stream->buf_cap = BUFSIZ;
    } else {
        size_t cap = (size > 0) ? size : BUFSIZ;
        char *b = (char *)malloc(cap);
        if (!b) {
            stream->buf_mode = _IONBF;
            stream->buf = NULL;
            stream->buf_cap = 0;
            return -1;
        }
        stream->buf = b;
        stream->buf_cap = cap;
        stream->own_buf = 1;
    }
    return 0;
}

FILE *fopen(const char *path, const char *mode)
{
    uint32_t rights = 0;
    if (mode[0] == 'r') rights = 1;
    else if (mode[0] == 'w') rights = 2 | 1;
    else if (mode[0] == 'a') rights = 2 | 1;

    a20_path_open_args_t args;
    args.size       = sizeof(args);
    args.version    = 1;
    args.dir        = A20_HANDLE_NULL;
    args.flags      = 0;
    args.rights     = rights;
    args.path       = (uint64_t)(uintptr_t)path;
    args.path_len   = (uint32_t)strlen(path);
    args.mode       = 0644;
    args.out_handle = A20_HANDLE_NULL;

    int64_t h = a20_syscall6(A20_SYS_path_open, (uint64_t)&args, 0, 0, 0, 0, 0);
    if (h < 0) return NULL;

    int fd = __fd_alloc(args.out_handle);
    if (fd < 0) {
        a20_syscall6(A20_SYS_handle_close, args.out_handle, 0, 0, 0, 0, 0);
        return NULL;
    }

    FILE *f = (FILE *)malloc(sizeof(FILE));
    if (!f) {
        __fd_free(fd);
        a20_syscall6(A20_SYS_handle_close, args.out_handle, 0, 0, 0, 0, 0);
        return NULL;
    }

    _file_init(f, args.out_handle, fd, mode[0] == 'r' ? 0 : 1);
    f->buf_mode = _IOFBF;
    f->own_buf = 1;
    f->buf = (char *)malloc(BUFSIZ);
    if (f->buf) {
        f->buf_cap = BUFSIZ;
    } else {
        f->buf_mode = _IONBF;
        f->buf_cap = 0;
    }
    return f;
}

int fclose(FILE *f)
{
    if (!f) return 0;
    _fflush_locked(f);
    a20_syscall6(A20_SYS_handle_close, f->handle, 0, 0, 0, 0, 0);
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
    if (total == 0) return 0;

    if (f->mode == 1)
        _fflush_locked(f);

    a20_iovec_t iov;
    iov.base = (uint64_t)(uintptr_t)ptr;
    iov.len  = total;

    a20_io_args_t args;
    args.size       = sizeof(args);
    args.version    = 1;
    args.handle     = f->handle;
    args._pad0      = 0;
    args.iov        = (uint64_t)&iov;
    args.iov_count  = 1;
    args._pad1      = 0;
    args.offset     = A20_OFFSET_CURRENT;
    args.out_count  = 0;

    int64_t r = a20_syscall6(A20_SYS_handle_read, (uint64_t)&args, 0, 0, 0, 0, 0);
    if (r < 0) {
        f->err = 1;
        return 0;
    }
    if (args.out_count == 0) f->eof = 1;
    return (size_t)args.out_count / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f)
{
    if (!f || !ptr || size == 0) return 0;
    size_t total = size * nmemb;
    if (total == 0) return 0;

    if (f->buf_mode == _IONBF || !f->buf || f->buf_cap == 0) {
        if (_write_all(f, (const char *)ptr, total) < 0) return 0;
        return nmemb;
    }

    size_t done = 0;
    while (done < total) {
        if (f->buf_len >= f->buf_cap) {
            if (_fflush_locked(f) < 0) { f->err = 1; break; }
        }

        size_t space = f->buf_cap - f->buf_len;
        size_t chunk = total - done;
        if (chunk > space) chunk = space;

        memcpy(f->buf + f->buf_len, (const char *)ptr + done, chunk);
        f->buf_len += chunk;
        done += chunk;

        if (f->buf_mode == _IOLBF) {
            int flush = 0;
            for (size_t i = 0; i < chunk; i++) {
                if (f->buf[f->buf_len - chunk + i] == '\n') {
                    flush = 1;
                    break;
                }
            }
            if (flush) _fflush_locked(f);
        }
    }

    return done / size;
}

int fputc(int c, FILE *f)
{
    char ch = (char)c;
    return (fwrite(&ch, 1, 1, f) == 1) ? (unsigned char)ch : EOF;
}

int fputs(const char *s, FILE *f)
{
    size_t len = strlen(s);
    return (fwrite(s, 1, len, f) == len) ? 0 : EOF;
}

int puts(const char *s)
{
    if (fputs(s, stdout) < 0) return EOF;
    return (fputc('\n', stdout) == '\n') ? 0 : EOF;
}

int fgetc(FILE *f)
{
    if (!f) return EOF;

    if (f->buf && f->buf_mode != _IONBF && f->buf_pos < f->buf_len) {
        return (unsigned char)f->buf[f->buf_pos++];
    }

    if (f->buf && f->buf_mode != _IONBF) {
        a20_iovec_t iov;
        iov.base = (uint64_t)(uintptr_t)f->buf;
        iov.len  = f->buf_cap;

        a20_io_args_t args;
        args.size       = sizeof(args);
        args.version    = 1;
        args.handle     = f->handle;
        args._pad0      = 0;
        args.iov        = (uint64_t)&iov;
        args.iov_count  = 1;
        args._pad1      = 0;
        args.offset     = A20_OFFSET_CURRENT;
        args.out_count  = 0;

        int64_t r = a20_syscall6(A20_SYS_handle_read, (uint64_t)&args, 0, 0, 0, 0, 0);
        if (r < 0) { f->err = 1; return EOF; }
        if (args.out_count == 0) { f->eof = 1; return EOF; }

        f->buf_len = (size_t)args.out_count;
        f->buf_pos = 0;
        return (unsigned char)f->buf[f->buf_pos++];
    }

    char c;
    a20_iovec_t iov;
    iov.base = (uint64_t)(uintptr_t)&c;
    iov.len  = 1;

    a20_io_args_t args;
    args.size       = sizeof(args);
    args.version    = 1;
    args.handle     = f->handle;
    args._pad0      = 0;
    args.iov        = (uint64_t)&iov;
    args.iov_count  = 1;
    args._pad1      = 0;
    args.offset     = A20_OFFSET_CURRENT;
    args.out_count  = 0;

    int64_t r = a20_syscall6(A20_SYS_handle_read, (uint64_t)&args, 0, 0, 0, 0, 0);
    if (r < 0) { f->err = 1; return EOF; }
    if (args.out_count == 0) { f->eof = 1; return EOF; }
    return (unsigned char)c;
}

char *fgets(char *s, int size, FILE *f)
{
    if (!f || !s || size <= 0) return NULL;

    int i = 0;
    while (i < size - 1) {
        int c = fgetc(f);
        if (c == EOF) break;
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return NULL;
    s[i] = '\0';
    return s;
}

long fseek(FILE *f, long offset, int whence)
{
    if (!f) return -1;
    _fflush_locked(f);
    a20_off_t off = (a20_off_t)offset;
    int64_t r = a20_syscall6(A20_SYS_handle_seek, f->handle,
                             (uint64_t)&off, (uint64_t)whence, 0, 0, 0);
    if (r < 0) { f->err = 1; return -1; }
    f->eof = 0;
    return 0;
}

int feof(FILE *f)
{
    return f ? f->eof : 0;
}

int ferror(FILE *f)
{
    return f ? f->err : 0;
}

void clearerr(FILE *f)
{
    if (f) { f->eof = 0; f->err = 0; }
}

static const char *_strerror(int err)
{
    switch (err) {
    case 0:  return "Success";
    case EPERM:        return "Operation not permitted";
    case ENOENT:       return "No such file or directory";
    case ESRCH:        return "No such process";
    case EINTR:        return "Interrupted system call";
    case EIO:          return "Input/output error";
    case ENXIO:        return "No such device or address";
    case E2BIG:        return "Argument list too long";
    case ENOEXEC:      return "Exec format error";
    case EBADF:        return "Bad file descriptor";
    case ECHILD:       return "No child processes";
    case EAGAIN:       return "Resource temporarily unavailable";
    case ENOMEM:       return "Cannot allocate memory";
    case EACCES:       return "Permission denied";
    case EFAULT:       return "Bad address";
    case EBUSY:        return "Device or resource busy";
    case EEXIST:       return "File exists";
    case EXDEV:        return "Invalid cross-device link";
    case ENODEV:       return "No such device";
    case ENOTDIR:      return "Not a directory";
    case EISDIR:       return "Is a directory";
    case EINVAL:       return "Invalid argument";
    case ENFILE:       return "Too many open files in system";
    case EMFILE:       return "Too many open files";
    case ENOTTY:       return "Inappropriate ioctl for device";
    case ENOSPC:       return "No space left on device";
    case ESPIPE:       return "Illegal seek";
    case EROFS:        return "Read-only file system";
    case EMLINK:       return "Too many links";
    case EPIPE:        return "Broken pipe";
    case ERANGE:       return "Numerical result out of range";
    case ENAMETOOLONG: return "File name too long";
    case ENOTEMPTY:    return "Directory not empty";
    case ENOTSUP:      return "Not supported";
    case EPROTO:       return "Protocol error";
    case ECANCELED:    return "Operation canceled";
    case ETIMEDOUT:    return "Connection timed out";
    default:           return "Unknown error";
    }
}

void perror(const char *s)
{
    if (s && *s) {
        fputs(s, stderr);
        fputs(": ", stderr);
    }
    fputs(_strerror(errno), stderr);
    fputc('\n', stderr);
}
