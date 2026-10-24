#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define HUGE_BYTES (2UL * 1024 * 1024)
#define MAP_BYTES (2UL * HUGE_BYTES)
#define PAGE_BYTES 4096UL

static int fail(const char *what)
{
    fprintf(stderr, "FORCE_HUGE_PS: FAIL %s (errno=%d)\n", what, errno);
    return 1;
}

static int read_knob(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, cap - 1);
    int saved = errno;
    close(fd);
    errno = saved;
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    return 0;
}

static int write_knob(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY | O_TRUNC);
    if (fd < 0)
        return -1;
    size_t len = strlen(value);
    ssize_t n = write(fd, value, len);
    int saved = errno;
    close(fd);
    errno = saved;
    return n == (ssize_t)len ? 0 : -1;
}

static unsigned long read_page_cache_pinned(void)
{
    char buf[1024];
    if (read_knob("/proc/a20/page_cache", buf, sizeof(buf)) < 0)
        return ULONG_MAX;
    char *field = strstr(buf, "pinned:");
    if (!field)
        return ULONG_MAX;
    return strtoul(field + strlen("pinned:"), NULL, 10);
}

#define STATM_PARSE_INVALID (-1)
#define STATM_PARSE_OVERFLOW (-2)

static int parse_statm_rss_kb(const char *text, unsigned long *rss_kb)
{
    if (!text || !rss_kb)
        return STATM_PARSE_INVALID;

    unsigned long values[7];
    const char *cursor = text;
    for (size_t i = 0; i < 7; i++) {
        while (isspace((unsigned char)*cursor))
            cursor++;
        if (*cursor < '0' || *cursor > '9')
            return STATM_PARSE_INVALID;

        errno = 0;
        char *end = NULL;
        unsigned long value = strtoul(cursor, &end, 10);
        if (end == cursor)
            return STATM_PARSE_INVALID;
        if (errno == ERANGE)
            return STATM_PARSE_OVERFLOW;
        if (*end && !isspace((unsigned char)*end))
            return STATM_PARSE_INVALID;
        values[i] = value;
        cursor = end;
    }
    while (isspace((unsigned char)*cursor))
        cursor++;
    if (*cursor != '\0')
        return STATM_PARSE_INVALID;

    unsigned long rss_pages = values[1];
    if (rss_pages > ULONG_MAX / (PAGE_BYTES / 1024))
        return STATM_PARSE_OVERFLOW;
    *rss_kb = rss_pages * (PAGE_BYTES / 1024);
    return 0;
}

static unsigned long read_vmrss_kb(void)
{
    char buf[256];
    if (read_knob("/proc/self/statm", buf, sizeof(buf)) < 0)
        return ULONG_MAX;

    unsigned long rss_kb = 0;
    int rc = parse_statm_rss_kb(buf, &rss_kb);
    if (rc == STATM_PARSE_INVALID) {
        fprintf(stderr, "FORCE_HUGE_PS: malformed /proc/self/statm: %s", buf);
        return ULONG_MAX;
    }
    if (rc == STATM_PARSE_OVERFLOW) {
        fprintf(stderr, "FORCE_HUGE_PS: /proc/self/statm RSS overflow: %s", buf);
        return ULONG_MAX;
    }
    return rss_kb;
}

static int mapping_has_anon_huge_page(uintptr_t address)
{
    FILE *fp = fopen("/proc/self/smaps", "r");
    if (!fp)
        return -1;
    char *line = NULL;
    size_t cap = 0;
    uintptr_t map_start = 0, map_end = 0;
    int in_target = 0;
    int found = 0;
    while (getline(&line, &cap, fp) >= 0) {
        unsigned long long start, end;
        if (sscanf(line, "%llx-%llx", &start, &end) == 2) {
            in_target = (address >= (uintptr_t)start &&
                         address < (uintptr_t)end);
            map_start = (uintptr_t)start;
            map_end = (uintptr_t)end;
            continue;
        }
        if (in_target && strncmp(line, "AnonHugePages:", 14) == 0) {
            unsigned long kb = 0;
            if (sscanf(line + 14, "%lu", &kb) == 1 && kb > 0)
                found = 1;
            break;
        }
    }
    free(line);
    fclose(fp);
    (void)map_start;
    (void)map_end;
    return found;
}

static unsigned char pattern(size_t index)
{
    return (unsigned char)((index * 29U + 0x35U) & 0xffU);
}

static int run_huge_regression(void)
{
    const unsigned long base_pins = read_page_cache_pinned();
    if (base_pins == ULONG_MAX)
        return fail("read-page-cache-pins-before");
    const unsigned long base_rss = read_vmrss_kb();
    if (base_rss == ULONG_MAX)
        return fail("read-proc-self-statm-before");

    char *raw = mmap(NULL, MAP_BYTES, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (raw == MAP_FAILED)
        return fail("map-4MiB-hugetlb");
    uintptr_t window_addr = ((uintptr_t)raw + HUGE_BYTES - 1) &
                            ~(uintptr_t)(HUGE_BYTES - 1);
    char *window = (char *)window_addr;
    if (window_addr + HUGE_BYTES > (uintptr_t)raw + MAP_BYTES) {
        munmap(raw, MAP_BYTES);
        return fail("aligned-window-outside-raw-mapping");
    }

    /* One store should fault in the huge block before the fork workload. */
    window[0] = 0x6d;
    int huge_pages = mapping_has_anon_huge_page(window_addr);
    if (huge_pages != 1) {
        munmap(raw, MAP_BYTES);
        return fail("smaps-anonhugepages-not-positive");
    }

    for (size_t i = 0; i < HUGE_BYTES; i++)
        window[i] = (char)pattern(i);

    pid_t child = fork();
    if (child < 0) {
        munmap(raw, MAP_BYTES);
        return fail("fork-cow");
    }
    if (child == 0) {
        for (size_t i = 0; i < HUGE_BYTES; i++)
            window[i] = (char)~pattern(i);
        _exit(0);
    }

    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        munmap(raw, MAP_BYTES);
        return fail("child-block-cow-status");
    }
    for (size_t i = 0; i < HUGE_BYTES; i++) {
        if ((unsigned char)window[i] != pattern(i)) {
            munmap(raw, MAP_BYTES);
            return fail("parent-pattern-changed-by-child");
        }
    }

    /* With the child gone, the parent's first writes should use its exclusive
     * mapping without changing the remaining pattern. */
    window[0] = 0xa1;
    window[HUGE_BYTES - 1] = 0xb2;
    if ((unsigned char)window[0] != 0xa1 ||
        (unsigned char)window[HUGE_BYTES - 1] != 0xb2) {
        munmap(raw, MAP_BYTES);
        return fail("parent-exclusive-write");
    }

    /* A partial protection change exercises the existing huge-to-base demote
     * path. A child write to that page must receive SIGSEGV. */
    char *protected_page = window + HUGE_BYTES / 2 + PAGE_BYTES;
    unsigned char protected_value = (unsigned char)*protected_page;
    if (mprotect(protected_page, PAGE_BYTES, PROT_READ) < 0) {
        munmap(raw, MAP_BYTES);
        return fail("partial-mprotect-readonly");
    }
    child = fork();
    if (child < 0) {
        munmap(raw, MAP_BYTES);
        return fail("fork-protection-probe");
    }
    if (child == 0) {
        *(volatile unsigned char *)protected_page =
            (unsigned char)(protected_value ^ 0xffU);
        _exit(0);
    }
    status = 0;
    if (waitpid(child, &status, 0) != child || !WIFSIGNALED(status) ||
        WTERMSIG(status) != SIGSEGV ||
        (unsigned char)*protected_page != protected_value) {
        munmap(raw, MAP_BYTES);
        return fail("partial-mprotect-signal-or-content");
    }

    /* Always unmap the original raw mapping, not the aligned inner window. */
    if (munmap(raw, MAP_BYTES) < 0)
        return fail("munmap-raw-mapping");
    unsigned long pins_after = read_page_cache_pinned();
    unsigned long rss_after = read_vmrss_kb();
    if (pins_after == ULONG_MAX)
        return fail("read-page-cache-pins-after");
    if (pins_after != base_pins)
        return fail("page-cache-pins-after-unmap");
    if (rss_after == ULONG_MAX)
        return fail("read-proc-self-statm-after");
    if (rss_after > base_rss + 1024)
        return fail("rss-after-unmap");

    printf("FORCE_HUGE_PS: PASS huge_pages=%d pins=%lu->%lu rss_kb=%lu->%lu\n",
           huge_pages, base_pins, pins_after, base_rss, rss_after);
    return 0;
}

int main(void)
{
    /* Warm libc/procfs parsing and smaps getline allocation before the RSS
     * baseline so measurement-side bookkeeping is not counted as a leak. */
    char warm_probe;
    (void)read_page_cache_pinned();
    (void)read_vmrss_kb();
    (void)mapping_has_anon_huge_page((uintptr_t)&warm_probe);

    const char *knob = "/proc/a20/anonprov";
    char saved[64];
    if (read_knob(knob, saved, sizeof(saved)) < 0)
        return fail("read-anonprov");
    if (write_knob(knob, "0\n") < 0)
        return fail("disable-anonprov");

    int result = run_huge_regression();
    if (write_knob(knob, saved) < 0) {
        fprintf(stderr, "FORCE_HUGE_PS: FAIL restore-anonprov\n");
        return 1;
    }
    return result;
}
