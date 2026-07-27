typedef unsigned long size_t;

extern char *getcwd(char *buffer, size_t size);
extern int chdir(const char *path);
extern int fchdir(int fd);
extern int open(const char *path, int flags, ...);
extern int close(int fd);
extern int *__errno_location(void);

#define SYS_GETCWD 17
#define SYS_WRITE 64
#define O_RDONLY 0
#define O_DIRECTORY 00200000

static long raw_syscall3(long number, long arg0, long arg1, long arg2)
{
#if defined(__riscv)
    register long a0 __asm__("a0") = arg0;
    register long a1 __asm__("a1") = arg1;
    register long a2 __asm__("a2") = arg2;
    register long a7 __asm__("a7") = number;
    __asm__ volatile("ecall"
                     : "+r"(a0)
                     : "r"(a1), "r"(a2), "r"(a7)
                     : "memory");
    return a0;
#elif defined(__loongarch__)
    register long a0 __asm__("$a0") = arg0;
    register long a1 __asm__("$a1") = arg1;
    register long a2 __asm__("$a2") = arg2;
    register long a7 __asm__("$a7") = number;
    __asm__ volatile("syscall 0"
                     : "+r"(a0)
                     : "r"(a1), "r"(a2), "r"(a7)
                     : "memory");
    return a0;
#else
#error unsupported BuildStorm probe architecture
#endif
}

static size_t string_length(const char *text)
{
    size_t length = 0;
    while (text[length] != '\0')
        length++;
    return length;
}

static void write_text(const char *text)
{
    raw_syscall3(SYS_WRITE, 1, (long)text, (long)string_length(text));
}

static void write_number(long value)
{
    char digits[32];
    size_t position = sizeof(digits);
    unsigned long magnitude;

    if (value < 0) {
        write_text("-");
        magnitude = (unsigned long)(-(value + 1)) + 1;
    } else {
        magnitude = (unsigned long)value;
    }
    do {
        digits[--position] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0);
    raw_syscall3(SYS_WRITE, 1, (long)&digits[position],
                 (long)(sizeof(digits) - position));
}

static int report_cwd_pair(const char *label)
{
    char raw_buffer[1024];
    char glibc_buffer[1024];
    long raw_result;
    char *glibc_result;

    write_text("BUILDSTORM_PROBE_CWD label=");
    write_text(label);
    write_text(" raw=");
    raw_result = raw_syscall3(SYS_GETCWD, (long)raw_buffer,
                              (long)sizeof(raw_buffer), 0);
    if (raw_result < 0) {
        write_text("ERROR:");
        write_number(raw_result);
    } else {
        write_text(raw_buffer);
        write_text(" raw_length=");
        write_number(raw_result);
    }

    write_text(" glibc=");
    glibc_result = getcwd(glibc_buffer, sizeof(glibc_buffer));
    if (glibc_result == (char *)0) {
        write_text("ERROR:");
        write_number(*__errno_location());
    } else {
        write_text(glibc_buffer);
    }
    write_text("\n");

    if (raw_result < 0 || glibc_result == (char *)0)
        return 1;
    return 0;
}

int probe_main(void)
{
    int saved_dir;
    int failed = report_cwd_pair("initial");

    saved_dir = open(".", O_RDONLY | O_DIRECTORY);
    if (saved_dir < 0) {
        write_text("BUILDSTORM_PROBE_FCHDIR open_failed errno=");
        write_number(*__errno_location());
        write_text("\n");
        return 1;
    }
    if (chdir("/") != 0) {
        write_text("BUILDSTORM_PROBE_FCHDIR chdir_failed errno=");
        write_number(*__errno_location());
        write_text("\n");
        close(saved_dir);
        return 1;
    }
    failed |= report_cwd_pair("after-chdir-root");
    if (fchdir(saved_dir) != 0) {
        write_text("BUILDSTORM_PROBE_FCHDIR fchdir_failed errno=");
        write_number(*__errno_location());
        write_text("\n");
        close(saved_dir);
        return 1;
    }
    close(saved_dir);
    failed |= report_cwd_pair("after-fchdir");
    return failed;
}
