/*
 * A20OS — Minimal User-mode C Library (libc)
 * Provides POSIX-compatible wrappers over raw syscalls.
 * Statically linked into each user-mode binary.
 */

#include "libc.h"

/* ============================================================
 * Syscall wrappers (assembly declared in syscall.S)
 * ============================================================ */

long syscall0(long num);
long syscall1(long num, long a0);
long syscall2(long num, long a0, long a1);
long syscall3(long num, long a0, long a1, long a2);
long syscall4(long num, long a0, long a1, long a2, long a3);
long syscall5(long num, long a0, long a1, long a2, long a3, long a4);
long syscall6(long num, long a0, long a1, long a2, long a3, long a4, long a5);

/* ---- errno --- */
int errno = 0;

#define SYSCALL_RET(r) do { if ((r) < 0) { errno = -(int)(r); return (typeof(r))-1; } return (r); } while(0)

/* ============================================================
 * Process
 * ============================================================ */

void _exit(int code) {
    syscall1(SYS_exit, code);
    while (1) {}
}

int getpid(void) {
    return (int)syscall0(SYS_getpid);
}

int getppid(void) {
    return (int)syscall0(SYS_getppid);
}

int fork(void) {
    /* clone with SIGCHLD */
    long r = syscall5(SYS_clone, 0x11 /* SIGCHLD */, 0, 0, 0, 0);
    SYSCALL_RET(r);
}

int waitpid(int pid, int *status, int options) {
    long r = syscall4(SYS_wait4, pid, (long)status, options, 0);
    SYSCALL_RET(r);
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    long r = syscall3(SYS_execve, (long)path, (long)argv, (long)envp);
    SYSCALL_RET(r);
}

int kill(int pid, int sig) {
    long r = syscall2(SYS_kill, pid, sig);
    SYSCALL_RET(r);
}

unsigned int sleep(unsigned int seconds) {
    struct timespec ts = { (long)seconds, 0 };
    syscall2(SYS_nanosleep, (long)&ts, 0);
    return 0;
}

int usleep(unsigned long usec) {
    struct timespec ts = { 0, (long)(usec * 1000) };
    return (int)syscall2(SYS_nanosleep, (long)&ts, 0);
}

/* ============================================================
 * File I/O
 * ============================================================ */

int open(const char *path, int flags, int mode) {
    long r = syscall4(SYS_openat, AT_FDCWD, (long)path, flags, mode);
    SYSCALL_RET(r);
}

int close(int fd) {
    long r = syscall1(SYS_close, fd);
    SYSCALL_RET(r);
}

ssize_t read(int fd, void *buf, size_t count) {
    long r = syscall3(SYS_read, fd, (long)buf, (long)count);
    SYSCALL_RET(r);
}

ssize_t write(int fd, const void *buf, size_t count) {
    long r = syscall3(SYS_write, fd, (long)buf, (long)count);
    SYSCALL_RET(r);
}

off_t lseek(int fd, off_t offset, int whence) {
    long r = syscall3(SYS_lseek, fd, offset, whence);
    SYSCALL_RET(r);
}

int dup(int fd) {
    long r = syscall1(SYS_dup, fd);
    SYSCALL_RET(r);
}

int dup2(int oldfd, int newfd) {
    long r = syscall3(SYS_dup3, oldfd, newfd, 0);
    SYSCALL_RET(r);
}

int pipe(int fds[2]) {
    long r = syscall2(SYS_pipe2, (long)fds, 0);
    SYSCALL_RET(r);
}

int fcntl(int fd, int cmd, int arg) {
    long r = syscall3(SYS_fcntl, fd, cmd, arg);
    SYSCALL_RET(r);
}

int ioctl(int fd, unsigned long req, void *arg) {
    long r = syscall3(SYS_ioctl, fd, (long)req, (long)arg);
    SYSCALL_RET(r);
}

int stat(const char *path, struct stat *st) {
    long r = syscall4(SYS_fstatat, AT_FDCWD, (long)path, (long)st, 0);
    SYSCALL_RET(r);
}

int fstat(int fd, struct stat *st) {
    long r = syscall2(SYS_fstat, fd, (long)st);
    SYSCALL_RET(r);
}

int access(const char *path, int mode) {
    long r = syscall3(SYS_faccessat, AT_FDCWD, (long)path, mode);
    SYSCALL_RET(r);
}

int unlink(const char *path) {
    long r = syscall3(SYS_unlinkat, AT_FDCWD, (long)path, 0);
    SYSCALL_RET(r);
}

int rename(const char *old, const char *new) {
    long r = syscall5(SYS_renameat2, AT_FDCWD, (long)old, AT_FDCWD, (long)new, 0);
    SYSCALL_RET(r);
}

int mkdir(const char *path, int mode) {
    long r = syscall3(SYS_mkdirat, AT_FDCWD, (long)path, mode);
    SYSCALL_RET(r);
}

int rmdir(const char *path) {
    long r = syscall3(SYS_unlinkat, AT_FDCWD, (long)path, AT_REMOVEDIR);
    SYSCALL_RET(r);
}

int chdir(const char *path) {
    long r = syscall1(SYS_chdir, (long)path);
    SYSCALL_RET(r);
}

char *getcwd(char *buf, size_t size) {
    long r = syscall2(SYS_getcwd, (long)buf, (long)size);
    if (r < 0) { errno = -(int)r; return NULL; }
    return buf;
}

int getdents(int fd, void *buf, int count) {
    long r = syscall3(SYS_getdents64, fd, (long)buf, (long)count);
    SYSCALL_RET(r);
}

/* ============================================================
 * Memory
 * ============================================================ */

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    long r = syscall6(SYS_mmap, (long)addr, (long)len, prot, flags, fd, (long)off);
    if (r < 0) { errno = -(int)r; return (void*)-1; }
    return (void*)r;
}

int munmap(void *addr, size_t len) {
    long r = syscall2(SYS_munmap, (long)addr, (long)len);
    SYSCALL_RET(r);
}

void *sbrk(intptr_t increment) {
    /* Get current brk */
    long cur = syscall1(SYS_brk, 0);
    if (increment == 0) return (void *)cur;
    long newbrk = syscall1(SYS_brk, cur + increment);
    if (newbrk < cur) { errno = ENOMEM; return (void *)-1; }
    return (void *)cur;
}

/* ============================================================
 * Minimal heap (malloc/free)
 * Simple bump allocator on top of sbrk()
 * ============================================================ */

typedef struct block_hdr {
    size_t size;
    int    free;
    struct block_hdr *next;
} block_hdr_t;

static block_hdr_t *heap_start = NULL;

void *malloc(size_t size) {
    if (size == 0) return NULL;
    size = (size + 7) & ~7UL; /* align to 8 */

    /* Search for free block (first-fit) */
    block_hdr_t *h = heap_start;
    while (h) {
        if (h->free && h->size >= size) {
            h->free = 0;
            return (void *)(h + 1);
        }
        h = h->next;
    }

    /* Extend heap */
    block_hdr_t *new_block = (block_hdr_t *)sbrk((intptr_t)(sizeof(block_hdr_t) + size));
    if (new_block == (void *)-1) return NULL;
    new_block->size = size;
    new_block->free = 0;
    new_block->next = NULL;

    /* Link into list */
    if (!heap_start) {
        heap_start = new_block;
    } else {
        block_hdr_t *last = heap_start;
        while (last->next) last = last->next;
        last->next = new_block;
    }
    return (void *)(new_block + 1);
}

void *calloc(size_t nmemb, size_t size) {
    void *p = malloc(nmemb * size);
    if (p) memset(p, 0, nmemb * size);
    return p;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    block_hdr_t *h = (block_hdr_t *)ptr - 1;
    if (h->size >= size) return ptr;
    void *new = malloc(size);
    if (!new) return NULL;
    memcpy(new, ptr, h->size);
    free(ptr);
    return new;
}

void free(void *ptr) {
    if (!ptr) return;
    block_hdr_t *h = (block_hdr_t *)ptr - 1;
    h->free = 1;
    /* Coalesce adjacent free blocks */
    block_hdr_t *cur = heap_start;
    while (cur && cur->next) {
        if (cur->free && cur->next->free) {
            cur->size += sizeof(block_hdr_t) + cur->next->size;
            cur->next = cur->next->next;
        } else {
            cur = cur->next;
        }
    }
}

/* ============================================================
 * String functions
 * ============================================================ */

size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (!n) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++));  return r; }
char *strncpy(char *d, const char *s, size_t n) {
    size_t i; for (i = 0; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = '\0'; return d;
}
char *strcat(char *d, const char *s) { char *r = d; while (*d) d++; while ((*d++ = *s++)); return r; }
char *strchr(const char *s, int c) { while (*s) { if (*s == (char)c) return (char*)s; s++; } return NULL; }
char *strrchr(const char *s, int c) { const char *l = NULL; while (*s) { if (*s == (char)c) l = s; s++; } return (char*)l; }
char *strstr(const char *h, const char *n) {
    size_t nl = strlen(n);
    if (!nl) return (char *)h;
    while (*h) { if (*h == *n && strncmp(h, n, nl) == 0) return (char*)h; h++; }
    return NULL;
}
static char *strtok_ptr = NULL;
char *strtok(char *s, const char *d) {
    if (!s) s = strtok_ptr;
    if (!s) return NULL;
    while (*s && strchr(d, *s)) s++;
    if (!*s) { strtok_ptr = NULL; return NULL; }
    char *tok = s;
    while (*s && !strchr(d, *s)) s++;
    if (*s) { *s = '\0'; strtok_ptr = s + 1; } else strtok_ptr = NULL;
    return tok;
}
void *memset(void *s, int c, size_t n) { unsigned char *p = s; while (n--) *p++ = (unsigned char)c; return s; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *dp = d; const unsigned char *sp = s; while (n--) *dp++ = *sp++; return d; }
void *memmove(void *d, const void *s, size_t n) {
    unsigned char *dp = d; const unsigned char *sp = s;
    if (dp < sp) { while (n--) *dp++ = *sp++; }
    else { dp += n; sp += n; while (n--) *--dp = *--sp; }
    return d;
}
int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *ap = a, *bp = b;
    while (n--) { if (*ap != *bp) return *ap - *bp; ap++; bp++; } return 0;
}
int atoi(const char *s) {
    int r = 0, sign = 1;
    while (*s == ' ') s++;
    if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') r = r * 10 + (*s++ - '0');
    return sign * r;
}
long atol(const char *s) {
    long r = 0; int sign = 1;
    while (*s == ' ') s++;
    if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') r = r * 10 + (*s++ - '0');
    return sign * r;
}
char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}
int strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        int ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* ============================================================
 * I/O: printf / putchar / puts / getchar
 * ============================================================ */

typedef __builtin_va_list va_list;
#define va_start(ap, param) __builtin_va_start(ap, param)
#define va_end(ap)          __builtin_va_end(ap)
#define va_arg(ap, type)    __builtin_va_arg(ap, type)

static char g_putbuf[4096];
static int  g_putpos = 0;

static void flush_putbuf(void) {
    if (g_putpos > 0) {
        write(1, g_putbuf, g_putpos);
        g_putpos = 0;
    }
}

int putchar(int c) {
    g_putbuf[g_putpos++] = (char)c;
    if (c == '\n' || g_putpos >= 4095) flush_putbuf();
    return c;
}

int puts(const char *s) {
    while (*s) putchar(*s++);
    putchar('\n');
    return 0;
}

static void print_uint(unsigned long v, int base, int upper) {
    char tmp[32]; int i = 0;
    if (v == 0) { putchar('0'); return; }
    while (v > 0) { int d = v % base; tmp[i++] = d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10; v /= base; }
    while (i-- > 0) putchar(tmp[i]);
}

static void print_int(long v) {
    if (v < 0) { putchar('-'); v = -v; }
    print_uint((unsigned long)v, 10, 0);
}

static int vsnprintf_impl(char *buf, size_t sz, const char *fmt, va_list ap);

int vprintf(const char *fmt, va_list ap) {
    char buf[4096];
    int n = vsnprintf_impl(buf, sizeof(buf), fmt, ap);
    if (n > 0) write(1, buf, n);
    return n;
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}

int snprintf(char *buf, size_t sz, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf_impl(buf, sz, fmt, ap);
    va_end(ap);
    return n;
}

static int vsnprintf_impl(char *buf, size_t sz, const char *fmt, va_list ap) {
    size_t pos = 0;
#define PUT(c) do { if (pos < sz - 1) buf[pos++] = (c); } while(0)
    while (*fmt) {
        if (*fmt != '%') { PUT(*fmt++); continue; }
        fmt++;
        int lng = 0, width = 0, pad_zero = 0;
        if (*fmt == '0') { pad_zero = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt++ - '0'); }
        while (*fmt == 'l') { lng++; fmt++; }

        char tmp[32]; int tl = 0;
        switch (*fmt) {
        case 'd': { long v = lng ? va_arg(ap, long) : va_arg(ap, int);
                    if (v < 0) { tmp[tl++] = '-'; v = -v; }
                    if (v == 0) { tmp[tl++] = '0'; } else {
                        char t2[24]; int i = 0; unsigned long uv = v;
                        while (uv) { t2[i++] = '0' + uv % 10; uv /= 10; }
                        while (i-- > 0) tmp[tl++] = t2[i]; }
                    break; }
        case 'u': { unsigned long v = lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
                    if (v == 0) { tmp[tl++] = '0'; } else {
                        char t2[24]; int i = 0;
                        while (v) { t2[i++] = '0' + v % 10; v /= 10; }
                        while (i-- > 0) tmp[tl++] = t2[i]; }
                    break; }
        case 'x': case 'X': { unsigned long v = lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
                    int up = (*fmt == 'X');
                    if (v == 0) { tmp[tl++] = '0'; } else {
                        char t2[24]; int i = 0;
                        while (v) { int d = v & 0xF; t2[i++] = d < 10 ? '0'+d : (up?'A':'a')+d-10; v >>= 4; }
                        while (i-- > 0) tmp[tl++] = t2[i]; }
                    break; }
        case 'p': { unsigned long v = (unsigned long)va_arg(ap, void*);
                    PUT('0'); PUT('x');
                    if (v == 0) { PUT('0'); } else {
                        char t2[24]; int i = 0;
                        while (v) { int d = v & 0xF; t2[i++] = d < 10 ? '0'+d : 'a'+d-10; v >>= 4; }
                        while (i-- > 0) tmp[tl++] = t2[i]; }
                    tl = 0; /* already printed */ goto next_fmt; }
        case 's': { const char *s = va_arg(ap, const char*); if (!s) s = "(null)";
                    int sl = (int)strlen(s);
                    for (int i = sl; i < width; i++) PUT(pad_zero ? '0' : ' ');
                    while (*s) PUT(*s++); goto next_fmt; }
        case 'c': { PUT((char)va_arg(ap, int)); goto next_fmt; }
        case '%': PUT('%'); goto next_fmt;
        default:  PUT('%'); PUT(*fmt); goto next_fmt;
        }
        /* Padding */
        for (int i = tl; i < width; i++) PUT(pad_zero ? '0' : ' ');
        for (int i = 0; i < tl; i++) PUT(tmp[i]);
next_fmt:
        fmt++;
    }
#undef PUT
    if (sz > 0) buf[pos] = '\0';
    return (int)pos;
}

int getchar(void) {
    char c;
    if (read(0, &c, 1) == 1) return (unsigned char)c;
    return -1;
}

char *fgets(char *buf, int size, int fd) {
    int i = 0;
    while (i < size - 1) {
        char c;
        int r = (int)read(fd, &c, 1);
        if (r <= 0) { if (i == 0) return NULL; break; }
        buf[i++] = c;
        if (c == '\n') break;
    }
    buf[i] = '\0';
    return (i > 0) ? buf : NULL;
}

/* ============================================================
 * Environment variables (simple array)
 * ============================================================ */

extern char **environ;
char **environ = NULL;

char *getenv(const char *name) {
    if (!environ) return NULL;
    size_t len = strlen(name);
    for (char **e = environ; *e; e++) {
        if (strncmp(*e, name, len) == 0 && (*e)[len] == '=')
            return (*e) + len + 1;
    }
    return NULL;
}

/* ============================================================
 * Signal
 * ============================================================ */

typedef void (*sighandler_t)(int);

int signal(int signum, sighandler_t handler) {
    struct { long h; long m; int f; } sa = { (long)handler, 0, 0 };
    return (int)syscall3(SYS_sigaction, signum, (long)&sa, 0);
}

/* ============================================================
 * Time
 * ============================================================ */

long time(long *t) {
    long r = syscall1(SYS_time, (long)t);
    return r;
}

/* ============================================================
 * Directory iteration
 * ============================================================ */

DIR *opendir(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) return NULL;
    DIR *d = malloc(sizeof(DIR));
    if (!d) { close(fd); return NULL; }
    d->fd  = fd;
    d->pos = 0;
    d->len = 0;
    return d;
}

struct dirent *readdir(DIR *d) {
    if (!d) return NULL;
    if (d->pos >= d->len) {
        int n = getdents(d->fd, d->buf, sizeof(d->buf));
        if (n <= 0) return NULL;
        d->len = n;
        d->pos = 0;
    }
    struct a20_dirent64 *ld = (struct a20_dirent64 *)(d->buf + d->pos);
    d->pos += ld->d_reclen;
    d->ent.d_type = ld->d_type;
    strncpy(d->ent.d_name, ld->d_name, 255);
    d->ent.d_name[255] = '\0';
    return &d->ent;
}

int closedir(DIR *d) {
    if (!d) return -1;
    close(d->fd);
    free(d);
    return 0;
}
