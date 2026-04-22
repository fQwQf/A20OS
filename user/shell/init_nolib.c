/* Contest init (no-libc): recursive scan + test script listing + poweroff */

typedef unsigned long  u64;
typedef long           s64;
typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

#define SYS_openat      56
#define SYS_close       57
#define SYS_getdents64  61
#define SYS_write       64
#define SYS_exit        93
#define SYS_reboot      142

#define AT_FDCWD        (-100)
#define O_RDONLY        0
#define DT_DIR          4

#define TEST_DIR        "/"
#define SUFFIX          "_testcode.sh"
#define MAX_TESTS       128
#define MAX_PATH        512

struct linux_dirent64_local {
    u64 d_ino;
    s64 d_off;
    u16 d_reclen;
    u8  d_type;
    char d_name[];
};

static inline long sys_call0(long n)
{
    register long a7 asm("a7") = n;
    register long a0 asm("a0");
    asm volatile ("ecall" : "=r"(a0) : "r"(a7) : "memory");
    return a0;
}

static inline long sys_call1(long n, long x0)
{
    register long a7 asm("a7") = n;
    register long a0 asm("a0") = x0;
    asm volatile ("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}

static inline long sys_call3(long n, long x0, long x1, long x2)
{
    register long a7 asm("a7") = n;
    register long a0 asm("a0") = x0;
    register long a1 asm("a1") = x1;
    register long a2 asm("a2") = x2;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

static long kstrlen(const char *s)
{
    long n = 0;
    while (s[n]) n++;
    return n;
}

static int kstrcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (int)((unsigned char)*a - (unsigned char)*b);
}

static int kends_with(const char *s, const char *suffix)
{
    long sl = kstrlen(s);
    long xl = kstrlen(suffix);
    if (xl > sl) return 0;
    return kstrcmp(s + sl - xl, suffix) == 0;
}

static void kcopy(char *dst, const char *src, long cap)
{
    long i = 0;
    if (cap <= 0) return;
    while (i < cap - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void kappend(char *dst, const char *src, long cap)
{
    long i = kstrlen(dst);
    long j = 0;
    if (i >= cap - 1) return;
    while (i < cap - 1 && src[j]) {
        dst[i++] = src[j++];
    }
    dst[i] = '\0';
}

static void put_str(const char *s)
{
    sys_call3(SYS_write, 1, (long)s, kstrlen(s));
}

static void put_nl(void)
{
    sys_call3(SYS_write, 1, (long)"\n", 1);
}

static void put_u32(u32 v)
{
    char buf[16];
    int i = 0;
    if (v == 0) {
        sys_call3(SYS_write, 1, (long)"0", 1);
        return;
    }
    while (v > 0 && i < (int)sizeof(buf)) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i > 0) {
        i--;
        sys_call3(SYS_write, 1, (long)&buf[i], 1);
    }
}

static void print_indent(int depth)
{
    for (int i = 0; i < depth; i++) put_str("  ");
}

static void join_path(char *out, const char *base, const char *name)
{
    kcopy(out, base, MAX_PATH);
    if (kstrcmp(base, "/") != 0) kappend(out, "/", MAX_PATH);
    kappend(out, name, MAX_PATH);
}

static char tests[MAX_TESTS][MAX_PATH];
static int test_count = 0;

static void scan_tree(const char *dir, int depth)
{
    char dbuf[2048];
    long fd = sys_call3(SYS_openat, AT_FDCWD, (long)dir, O_RDONLY);
    if (fd < 0) {
        print_indent(depth);
        put_str("[DIR] ");
        put_str(dir);
        put_str(" (open failed)");
        put_nl();
        return;
    }

    for (;;) {
        long nread = sys_call3(SYS_getdents64, fd, (long)dbuf, (long)sizeof(dbuf));
        if (nread <= 0) break;

        long bpos = 0;
        while (bpos < nread) {
            struct linux_dirent64_local *de = (struct linux_dirent64_local *)(dbuf + bpos);
            if (de->d_reclen == 0) break;

            const char *name = de->d_name;
            if (kstrcmp(name, ".") != 0 && kstrcmp(name, "..") != 0) {
                char full[MAX_PATH];
                int is_dir = (de->d_type == DT_DIR);
                join_path(full, dir, name);

                print_indent(depth);
                put_str(is_dir ? "[D] " : "[F] ");
                put_str(full);
                put_nl();

                if (!is_dir && kends_with(name, SUFFIX) && test_count < MAX_TESTS) {
                    kcopy(tests[test_count], full, MAX_PATH);
                    test_count++;
                }

                if (is_dir) scan_tree(full, depth + 1);
            }

            bpos += de->d_reclen;
        }
    }

    sys_call1(SYS_close, fd);
}

static int main(void)
{
    put_str("[CONTEST] Scan-only init started\n");
    put_str("[CONTEST] Scanning target image at ");
    put_str(TEST_DIR);
    put_nl();

    scan_tree(TEST_DIR, 0);

    put_str("[CONTEST] Found ");
    put_u32((u32)test_count);
    put_str(" test script(s) matching ");
    put_str(SUFFIX);
    put_nl();

    for (int i = 0; i < test_count; i++) {
        put_str("[CONTEST] TEST[");
        put_u32((u32)i);
        put_str("] ");
        put_str(tests[i]);
        put_nl();
    }

    put_str("[CONTEST] Scan complete, powering off\n");
    sys_call1(SYS_reboot, 0);
    return 0;
}

void _start(void)
{
    int code = main();
    sys_call1(SYS_exit, code);
    for (;;) {}
}

