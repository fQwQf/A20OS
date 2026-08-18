#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TIMEOUT_MS 200
#define PERF_PATH "/proc/a20/perf"

static uint64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int parse_counter(const char *buf, const char *name, uint64_t *value)
{
    const char *line = strstr(buf, name);
    if (!line)
        return -1;
    line += strlen(name);
    if (*line++ != ':')
        return -1;
    while (*line == ' ' || *line == '\t')
        line++;

    char *end;
    unsigned long long parsed = strtoull(line, &end, 10);
    if (end == line)
        return -1;
    *value = (uint64_t)parsed;
    return 0;
}

static int read_idle_counters(uint64_t *entries, uint64_t *wake_returns)
{
    char buf[8192];
    int fd = open(PERF_PATH, O_RDONLY);
    if (fd < 0)
        return -1;

    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    if (n <= 0)
        return -1;
    buf[n] = '\0';

    if (parse_counter(buf, "idle_wait_entries", entries) < 0 ||
        parse_counter(buf, "idle_wait_wake_returns", wake_returns) < 0) {
        errno = EPROTO;
        return -1;
    }
    return 0;
}

static int disable_perf_counters(void)
{
    int fd = open(PERF_PATH, O_WRONLY);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, "0", 1);
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return n == 1 ? 0 : -1;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("TIMER_IDLE: start\n");

    uint64_t entries_before;
    uint64_t wakes_before;
    if (read_idle_counters(&entries_before, &wakes_before) < 0) {
        printf("TIMER_IDLE: FAIL perf-before errno=%d\n", errno);
        return 1;
    }

    uint64_t started = now_ns();
    if (!started) {
        (void)disable_perf_counters();
        printf("TIMER_IDLE: FAIL clock-before errno=%d\n", errno);
        return 1;
    }

    errno = 0;
    int timeout_rc = poll(NULL, 0, TIMEOUT_MS);
    int timeout_errno = errno;
    uint64_t finished = now_ns();
    if (!finished) {
        (void)disable_perf_counters();
        printf("TIMER_IDLE: FAIL clock-after errno=%d\n", errno);
        return 1;
    }

    uint64_t entries_after;
    uint64_t wakes_after;
    if (read_idle_counters(&entries_after, &wakes_after) < 0) {
        (void)disable_perf_counters();
        printf("TIMER_IDLE: FAIL perf-after errno=%d\n", errno);
        return 1;
    }
    if (disable_perf_counters() < 0) {
        printf("TIMER_IDLE: FAIL perf-disable errno=%d\n", errno);
        return 1;
    }

    uint64_t elapsed_ms = (finished - started) / 1000000ULL;
    uint64_t entry_delta = entries_after - entries_before;
    uint64_t wake_delta = wakes_after - wakes_before;
    int passed = timeout_rc == 0 && elapsed_ms >= 150 &&
                 elapsed_ms <= 2000 && entry_delta > 0 && wake_delta > 0;

    printf("TIMER_IDLE: timeout_rc=%d errno=%d elapsed_ms=%llu "
           "idle_entries_delta=%llu idle_wake_delta=%llu\n",
           timeout_rc, timeout_errno,
           (unsigned long long)elapsed_ms,
           (unsigned long long)entry_delta,
           (unsigned long long)wake_delta);
    printf("TIMER_IDLE: %s\n", passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
