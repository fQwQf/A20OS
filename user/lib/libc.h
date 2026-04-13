#ifndef _LIBC_H
#define _LIBC_H

/* ============================================================
 * A20OS User-mode Library Header
 * ============================================================ */

typedef unsigned long  size_t;
typedef long           ssize_t;
typedef long           off_t;
typedef long           intptr_t;
typedef unsigned long  uintptr_t;
typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;
typedef unsigned long  uint64_t;
typedef signed long    int64_t;
typedef signed int     int32_t;

typedef _Bool bool;
#define true  1
#define false 0
#define NULL  ((void *)0)

/* ---- Error codes ---- */
extern int errno;
#define EPERM   1
#define ENOENT  2
#define ESRCH   3
#define EINTR   4
#define EIO     5
#define ENOEXEC 8
#define EBADF   9
#define ECHILD  10
#define EAGAIN  11
#define ENOMEM  12
#define EACCES  13
#define EFAULT  14
#define EBUSY   16
#define EEXIST  17
#define EINVAL  22
#define ENOTDIR 20
#define EISDIR  21
#define EMFILE  24
#define ENOSPC  28
#define EPIPE   32
#define ENOSYS  38

/* ---- Syscall numbers (RISC-V / Linux ABI) ---- */
#define SYS_read        63
#define SYS_write       64
#define SYS_openat      56
#define SYS_close       57
#define SYS_lseek       62
#define SYS_fstat       80
#define SYS_fstatat     79
#define SYS_exit        93
#define SYS_exit_group  94
#define SYS_getpid      172
#define SYS_getppid     173
#define SYS_clone       220
#define SYS_execve      221
#define SYS_wait4       260
#define SYS_kill        129
#define SYS_nanosleep   101
#define SYS_pipe2       59
#define SYS_dup         23
#define SYS_dup3        24
#define SYS_fcntl       25
#define SYS_ioctl       29
#define SYS_mkdirat     34
#define SYS_unlinkat    35
#define SYS_renameat2   276
#define SYS_chdir       49
#define SYS_getcwd      17
#define SYS_getdents64  61
#define SYS_brk         214
#define SYS_mmap        222
#define SYS_munmap      215
#define SYS_mprotect    226
#define SYS_clock_gettime 113
#define SYS_gettimeofday  169
#define SYS_time        235
#define SYS_uname       160
#define SYS_sysinfo     179
#define SYS_sched_yield 124
#define SYS_umask       166
#define SYS_getrandom   278
#define SYS_sigaction   134
#define SYS_sigprocmask 135
#define SYS_prctl       167
#define SYS_faccessat   48
#define SYS_reboot      142
#define SYS_statfs      43
#define SYS_writev      66
#define SYS_readlinkat  78
#define SYS_mount       40
#define SYS_umount2     39
#define SYS_fsync       82
#define SYS_truncate    45
#define SYS_ftruncate   46
#define SYS_pread64     67
#define SYS_pwrite64    68
#define SYS_ppoll       73
#define SYS_sendfile    71
#define SYS_getuid      174
#define SYS_geteuid     175
#define SYS_getgid      176
#define SYS_getegid     177

/* ---- File flags ---- */
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     0x100
#define O_TRUNC     0x200
#define O_APPEND    0x400
#define O_NONBLOCK  0x800
#define O_DIRECTORY 0x10000
#define O_CLOEXEC   0x80000

#define AT_FDCWD    (-100)
#define AT_REMOVEDIR 0x200

#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

#define TIOCGWINSZ  0x5413

/* ---- stat ---- */
struct stat {
    unsigned long st_dev;
    unsigned long st_ino;
    unsigned int  st_mode;
    unsigned int  st_nlink;
    unsigned int  st_uid;
    unsigned int  st_gid;
    unsigned long st_size;
    unsigned long st_blksize;
    unsigned long st_blocks;
    long          st_atime;
    long          st_mtime;
    long          st_ctime;
};

/* ---- Mode bits ---- */
#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_IFLNK  0120000
#define S_IXUSR  0100
#define S_IWUSR  0200
#define S_IRUSR  0400

#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)

/* ---- linux_dirent64 ---- */
struct linux_dirent64 {
    unsigned long  d_ino;
    long           d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[256];
} __attribute__((packed));

/* ---- dirent / DIR ---- */
struct dirent {
    unsigned char d_type;
    char          d_name[256];
};
#define DT_UNKNOWN  0
#define DT_FIFO     1
#define DT_CHR      2
#define DT_DIR      4
#define DT_BLK      6
#define DT_REG      8
#define DT_LNK      10
#define DT_SOCK     12

typedef struct {
    int   fd;
    int   pos;
    int   len;
    char  buf[4096];
    struct dirent ent;
} DIR;

/* ---- timespec ---- */
struct timespec {
    long tv_sec;
    long tv_nsec;
};

/* ---- Signal numbers ---- */
#define SIGINT   2
#define SIGKILL  9
#define SIGTERM  15
#define SIGCHLD  17
#define SIGPIPE  13
#define SIGALRM  14
#define SIGUSR1  10
#define SIGUSR2  12
#define SIGSTOP  19
#define SIGCONT  18
#define SIGHUP   1

/* ---- Wait options ---- */
#define WNOHANG    1
#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)
#define WIFEXITED(s)   (((s) & 0x7F) == 0)

/* ============================================================
 * Function declarations
 * ============================================================ */

/* Process */
void   _exit(int code) __attribute__((noreturn));
int    getpid(void);
int    getppid(void);
int    fork(void);
int    waitpid(int pid, int *status, int options);
int    execve(const char *path, char *const argv[], char *const envp[]);
int    kill(int pid, int sig);
unsigned int sleep(unsigned int sec);
int    usleep(unsigned long usec);

/* File I/O */
int    open(const char *path, int flags, int mode);
int    close(int fd);
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
off_t  lseek(int fd, off_t offset, int whence);
int    dup(int fd);
int    dup2(int oldfd, int newfd);
int    pipe(int fds[2]);
int    fcntl(int fd, int cmd, int arg);
int    ioctl(int fd, unsigned long req, void *arg);
int    stat(const char *path, struct stat *st);
int    fstat(int fd, struct stat *st);
int    access(const char *path, int mode);
int    unlink(const char *path);
int    rename(const char *old, const char *newp);
int    mkdir(const char *path, int mode);
int    rmdir(const char *path);
int    chdir(const char *path);
char  *getcwd(char *buf, size_t size);
int    getdents(int fd, void *buf, int count);

/* Memory */
void  *malloc(size_t size);
void  *calloc(size_t n, size_t size);
void  *realloc(void *p, size_t size);
void   free(void *p);
void  *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
int    munmap(void *addr, size_t len);
void  *sbrk(intptr_t increment);

/* String */
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *d, const char *s);
char  *strncpy(char *d, const char *s, size_t n);
char  *strcat(char *d, const char *s);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *h, const char *n);
char  *strtok(char *s, const char *d);
void  *memset(void *s, int c, size_t n);
void  *memcpy(void *d, const void *s, size_t n);
void  *memmove(void *d, const void *s, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
int    atoi(const char *s);
long   atol(const char *s);
char  *strdup(const char *s);
int    strcasecmp(const char *a, const char *b);

/* I/O */
typedef __builtin_va_list va_list;
#define va_start(ap, p) __builtin_va_start(ap, p)
#define va_end(ap)      __builtin_va_end(ap)
#define va_arg(ap, t)   __builtin_va_arg(ap, t)

int    printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int    snprintf(char *buf, size_t sz, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int    vprintf(const char *fmt, va_list ap);
int    putchar(int c);
int    puts(const char *s);
int    getchar(void);
char  *fgets(char *buf, int size, int fd);

/* Environment */
extern char **environ;
char  *getenv(const char *name);

/* Raw syscall wrappers (from syscall.S) */
long   syscall0(long num);
long   syscall1(long num, long a0);
long   syscall2(long num, long a0, long a1);
long   syscall3(long num, long a0, long a1, long a2);
long   syscall4(long num, long a0, long a1, long a2, long a3);
long   syscall5(long num, long a0, long a1, long a2, long a3, long a4);
long   syscall6(long num, long a0, long a1, long a2, long a3, long a4, long a5);

/* Signal */
typedef void (*sighandler_t)(int);
int    signal(int signum, sighandler_t handler);

/* Time */
long   time(long *t);

/* Directory */
DIR        *opendir(const char *path);
struct dirent *readdir(DIR *d);
int         closedir(DIR *d);

#endif /* _LIBC_H */
