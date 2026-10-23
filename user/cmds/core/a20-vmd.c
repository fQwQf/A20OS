/* A20 VM supervisor. Port 0 is a dedicated virtio-serial control channel.
 * Children run hypvm --managed; disconnecting the host never closes their
 * pipes. Bounded rings retain console history without blocking guest output. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "hyp/vmd_protocol.h"
#include "hyp/hyp_guest.h"
#include <drivers/char/virtio_console.h>
#define HYP_MAX_GUEST_CPUS HYP_GUEST_MAX_VCPUS

struct guest {
    char name[VMD_NAME], kernel[VMD_PATH];
    pid_t pid;
    unsigned state, memory_mb, vcpus;
    int exit_code, input_fd, output_fd;
    uint64_t output_end, stop_deadline, generation;
    uint8_t history[VMD_HISTORY], input[VMD_INPUT];
    unsigned input_begin, input_end;
};
static struct guest guests[VMD_MAX_GUESTS];
static volatile sig_atomic_t shutting_down;
static int control_fd = -1;
struct cached {
    uint64_t id;
    uint32_t crc;
    size_t len;
    uint8_t response[VMD_FRAME], request[VMD_FRAME];
    size_t request_len;
};
static struct cached replies[32];
static unsigned reply_slot;
static uint64_t manager_epoch, generation_count;
static uint8_t received[VMD_FRAME];
static int escaped, discard_frame;
static size_t received_len;

static uint64_t now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) return 0;
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
static void on_signal(int sig) { (void)sig; shutting_down = 1; }
static int nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
static int valid_name(const uint8_t *p)
{
    size_t n = strnlen((const char *)p, VMD_NAME);
    if (!n || n == VMD_NAME) return 0;
    for (size_t i = 0; i < n; i++)
        if (!(p[i] >= 'a' && p[i] <= 'z') &&
            !(p[i] >= 'A' && p[i] <= 'Z') &&
            !(p[i] >= '0' && p[i] <= '9') && p[i] != '_' && p[i] != '-')
            return 0;
    /* Reject noncanonical padding: one name has one encoding. */
    for (size_t i = n; i < VMD_NAME; i++) if (p[i]) return 0;
    return 1;
}
static struct guest *find_guest(const uint8_t *name)
{
    for (unsigned i = 0; i < VMD_MAX_GUESTS; i++)
        if (guests[i].name[0] && !strcmp(guests[i].name, (const char *)name))
            return &guests[i];
    return NULL;
}
static void close_pipes(struct guest *g)
{
    if (g->input_fd >= 0) close(g->input_fd);
    if (g->output_fd >= 0) close(g->output_fd);
    g->input_fd = g->output_fd = -1;
}
static int start_guest(const uint8_t *p)
{
    unsigned memory = (unsigned)vmd_get(p + VMD_NAME, 4);
    unsigned vcpus = (unsigned)vmd_get(p + VMD_NAME + 4, 4);
    const char *kernel = (const char *)p + VMD_NAME + 8;
    if (!vcpus || vcpus > HYP_MAX_GUEST_CPUS || memory < 8 || memory > 256 ||
        !memchr(kernel, 0, VMD_PATH) || strncmp(kernel, "/bin/boot/", 10) ||
        strstr(kernel, "..") || !kernel[10]) return -EINVAL;
    /* Boot artifacts, never executable paths. No shell parsing is involved. */
    for (const char *c = kernel + 10; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
              (*c >= '0' && *c <= '9') || *c == '.' || *c == '_' || *c == '-'))
            return -EINVAL;
    struct guest *same = find_guest(p);
    if (same)
        return same->memory_mb == memory && same->vcpus == vcpus && !strcmp(same->kernel, kernel) ?
               0 : -EEXIST;
    unsigned total = 0;
    struct guest *g = NULL;
    for (unsigned i = 0; i < VMD_MAX_GUESTS; i++) {
        if (!guests[i].name[0]) g = &guests[i];
        if (guests[i].pid > 0) total += guests[i].memory_mb;
    }
    if (!g || total + memory > VMD_MEMORY_MB) return -ENOSPC;
    if (access(kernel, R_OK) < 0) return -errno;
    int input[2], output[2];
    if (pipe(input) < 0) return -errno;
    if (pipe(output) < 0) {
        int error = errno;
        close(input[0]); close(input[1]);
        return -error;
    }
    /* Both ends are close-on-exec; only explicitly duplicated stdio survives. */
    for (unsigned i = 0; i < 2; i++) {
        fcntl(input[i], F_SETFD, FD_CLOEXEC);
        fcntl(output[i], F_SETFD, FD_CLOEXEC);
    }
    if (nonblock(input[1]) < 0 || nonblock(output[0]) < 0) {
        int error = errno;
        close(input[0]); close(input[1]); close(output[0]); close(output[1]);
        return -error;
    }
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid < 0) {
        int error = errno;
        close(input[0]); close(input[1]); close(output[0]); close(output[1]);
        return -error;
    }
    if (!pid) {
        if (prctl(PR_SET_PDEATHSIG, SIGKILL, 0L, 0L, 0L) < 0 || getppid() != parent)
            _exit(125);
        setsid();
        if (dup2(input[0], STDIN_FILENO) < 0 ||
            dup2(output[1], STDOUT_FILENO) < 0 ||
            dup2(output[1], STDERR_FILENO) < 0) _exit(126);
        close(input[0]); close(input[1]); close(output[0]); close(output[1]);
        /* Close other guests' inherited pipe ends, preventing false EOF. */
        for (unsigned i = 0; i < VMD_MAX_GUESTS; i++) close_pipes(&guests[i]);
        if (control_fd >= 0) close(control_fd);
        char mb[16], cpu_count[16];
        snprintf(mb, sizeof(mb), "%u", memory);
        snprintf(cpu_count, sizeof(cpu_count), "%u", vcpus);
        char *const argv[] = { "hypvm", "--managed", "-k", (char *)kernel,
                               "-m", mb, "--smp", cpu_count, NULL };
        execv("/bin/hypvm", argv);
        perror("a20-vmd: exec hypvm");
        _exit(127);
    }
    close(input[0]); close(output[1]);
    memset(g, 0, sizeof(*g));
    memcpy(g->name, p, VMD_NAME);
    snprintf(g->kernel, sizeof(g->kernel), "%s", kernel);
    g->generation = ++generation_count;
    g->pid = pid; g->state = VMD_RUNNING; g->memory_mb = memory; g->vcpus = vcpus;
    g->input_fd = input[1]; g->output_fd = output[0];
    return 0;
}
static void stop_guest(struct guest *g)
{
    if (g->pid <= 0 || g->state == VMD_STOPPING) return;
    /* A stopped VMM must be resumed before SIGTERM can reach a safe point. */
    kill(g->pid, SIGCONT);
    kill(g->pid, SIGTERM);
    g->state = VMD_STOPPING;
    g->stop_deadline = now_ms() + 2000;
}
static int dispatch(uint16_t op, const uint8_t *p, size_t len,
                    uint8_t *data, size_t *out_len)
{
    *out_len = 0;
    if (op == VMD_HELLO) {
        if (len) return -EINVAL;
        vmd_put(data, VMD_VERSION, 4); vmd_put(data + 4, VMD_MAX_GUESTS, 4);
        vmd_put(data + 8, VMD_MEMORY_MB, 4);
        vmd_put(data + 12, manager_epoch, 8); *out_len = 20;
        return 0;
    }
    if (len < 8 || vmd_get(p, 8) != manager_epoch) return -ESTALE;
    p += 8; len -= 8;
    if (op == VMD_STATS) {
        if (len) return -EINVAL;
        struct hyp_guest_stats stats;
        if (hyp_guest_get_stats(&stats) < 0) return -errno;
        vmd_put(data, stats.vms, 8); vmd_put(data + 8, stats.vcpus, 8);
        vmd_put(data + 16, stats.mapped_pages, 8);
        vmd_put(data + 24, stats.table_pages, 8);
        vmd_put(data + 32, stats.free_host_pages, 8);
        *out_len = 40;
        return 0;
    }
    if (op == VMD_LIST) {
        if (len) return -EINVAL;
        for (unsigned i = 0; i < VMD_MAX_GUESTS; i++) {
            struct guest *g = &guests[i];
            if (!g->name[0]) continue;
            uint8_t *record = data + *out_len;
            memcpy(record, g->name, VMD_NAME);
            vmd_put(record + 32, g->pid, 4); vmd_put(record + 36, g->state, 4);
            vmd_put(record + 40, (uint32_t)g->exit_code, 4);
            vmd_put(record + 44, g->memory_mb, 4);
            vmd_put(record + 48, g->generation, 8);
            vmd_put(record + 56, g->output_end, 8);
            vmd_put(record + 64, g->vcpus, 4); vmd_put(record + 68, 0, 4);
            *out_len += VMD_RECORD;
        }
        return 0;
    }
    if (len < VMD_NAME || !valid_name(p)) return -EINVAL;
    if (op == VMD_START) {
        if (len != VMD_START_SIZE) return -EINVAL;
        int rc = start_guest(p);
        if (!rc) {
            struct guest *g = find_guest(p);
            vmd_put(data, g->generation, 8); *out_len = 8;
        }
        return rc;
    }
    struct guest *g = find_guest(p);
    if (!g) return op == VMD_DESTROY && len == VMD_NAME + 8 ? 0 : -ENOENT;
    if (len < VMD_NAME + 8 || vmd_get(p + VMD_NAME, 8) != g->generation)
        return -ESTALE;
    if (op == VMD_WRITE) {
        if (g->pid <= 0 || g->state == VMD_STOPPING) return -EPIPE;
        size_t accepted = 0;
        while (VMD_NAME + 8 + accepted < len) {
            unsigned next = (g->input_end + 1) % VMD_INPUT;
            if (next == g->input_begin) break;
            g->input[g->input_end] = p[VMD_NAME + 8 + accepted++];
            g->input_end = next;
        }
        vmd_put(data, accepted, 4); *out_len = 4;
        return 0;
    }
    if (op == VMD_READ) {
        if (len != VMD_NAME + 20) return -EINVAL;
        uint64_t cursor = vmd_get(p + VMD_NAME + 8, 8);
        unsigned limit = (unsigned)vmd_get(p + VMD_NAME + 16, 4);
        uint64_t oldest = g->output_end > VMD_HISTORY ? g->output_end - VMD_HISTORY : 0;
        unsigned gap = cursor < oldest;
        if (cursor < oldest) cursor = oldest;
        if (cursor > g->output_end) return -ERANGE;
        if (limit > VMD_PAYLOAD - 24) limit = VMD_PAYLOAD - 24;
        uint64_t start = cursor;
        unsigned n = 0;
        while (n < limit && cursor < g->output_end)
            data[20 + n++] = g->history[cursor++ % VMD_HISTORY];
        vmd_put(data, start, 8); vmd_put(data + 8, cursor, 8);
        vmd_put(data + 16, gap, 4); *out_len = 20 + n;
        return 0;
    }
    if (len != VMD_NAME + 8) return -EINVAL;
    if (op == VMD_DESTROY) {
        if (g->pid > 0) return -EBUSY;
        close_pipes(g); memset(g, 0, sizeof(*g));
        g->input_fd = g->output_fd = -1;
        return 0;
    }
    if (op == VMD_STOP) { stop_guest(g); return 0; }
    if (g->pid <= 0) return -ESRCH;
    if (g->state == VMD_STOPPING) return -EBUSY;
    if (op == VMD_PAUSE) {
        if (g->state == VMD_PAUSED || g->state == VMD_PAUSING) return 0;
        if (kill(g->pid, SIGSTOP) < 0) return -errno;
        g->state = VMD_PAUSING;
        return 0;
    }
    if (op == VMD_RESUME) {
        if (kill(g->pid, SIGCONT) < 0) return -errno;
        g->state = VMD_RUNNING;
        return 0;
    }
    return -EINVAL;
}
static int send_frame(const uint8_t *p, size_t len)
{
    uint8_t wire[VMD_FRAME * 2 + 2];
    size_t nwire = 0;
    wire[nwire++] = 0x7e;
    for (size_t i = 0; i < len; i++) {
        if (p[i] == 0x7e || p[i] == 0x7d) {
            wire[nwire++] = 0x7d; wire[nwire++] = p[i] ^ 0x20;
        } else wire[nwire++] = p[i];
    }
    wire[nwire++] = 0x7e;
    size_t sent = 0;
    uint64_t deadline = now_ms() + 2000;
    while (sent < nwire) {
        ssize_t n = write(control_fd, wire + sent, nwire - sent);
        if (n > 0) { sent += (size_t)n; continue; }
        if (n < 0 && errno != EAGAIN && errno != EINTR) return -1;
        if (now_ms() >= deadline) return -1;
        struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL);
    }
    return 0;
}
static void request(const uint8_t *p)
{
    uint64_t id = vmd_get(p + 8, 8);
    uint16_t op = (uint16_t)vmd_get(p + 6, 2);
    uint32_t crc = (uint32_t)vmd_get(p + 20, 4);
    uint8_t payload[VMD_PAYLOAD], frame[VMD_FRAME];
    for (unsigned i = 0; i < 32; i++) {
        if (!replies[i].len || replies[i].id != id) continue;
        size_t request_len = VMD_HEADER + (size_t)vmd_get(p + 16, 4);
        if (replies[i].crc == crc && replies[i].request_len == request_len &&
            !memcmp(replies[i].request, p, request_len)) {
            send_frame(replies[i].response, replies[i].len);
            return;
        }
        vmd_put(payload, (uint32_t)-EPROTO, 4);
        send_frame(frame, vmd_frame(frame, op | VMD_RESPONSE, id, payload, 4));
        return;
    }
    size_t size;
    int rc = dispatch(op, p + VMD_HEADER, (size_t)vmd_get(p + 16, 4), payload + 4, &size);
    vmd_put(payload, (uint32_t)rc, 4);
    size = vmd_frame(frame, op | VMD_RESPONSE, id, payload, size + 4);
    /* Publish deduplication before transmission: a lost reply must not repeat
     * side effects. The cache is volatile and bounded, not crash recovery. */
    struct cached *cache = &replies[reply_slot++ % 32];
    cache->id = id; cache->crc = crc; cache->len = size;
    memcpy(cache->response, frame, size);
    cache->request_len = VMD_HEADER + (size_t)vmd_get(p + 16, 4);
    memcpy(cache->request, p, cache->request_len);
    send_frame(frame, size);
}
static int complete_frame(const uint8_t *p, size_t len)
{
    if (len < VMD_HEADER || memcmp(p, "A2VM", 4)) return 0;
    size_t size = (size_t)vmd_get(p + 16, 4);
    if (vmd_get(p + 4, 2) != VMD_VERSION || size > VMD_PAYLOAD ||
        (vmd_get(p + 6, 2) & VMD_RESPONSE) || !vmd_get(p + 8, 8) ||
        len != VMD_HEADER + size) return 0;
    return vmd_get(p + 20, 4) == vmd_checksum(p, size);
}
static void receive_requests(void)
{
    uint8_t wire[2048];
    ssize_t n = read(control_fd, wire, sizeof(wire));
    for (ssize_t i = 0; i < n; i++) {
        uint8_t c = wire[i];
        if (c == 0x7e) {
            if (!discard_frame && !escaped && complete_frame(received, received_len))
                request(received);
            received_len = 0; escaped = discard_frame = 0;
            continue;
        }
        if (discard_frame) continue;
        if (escaped) { c ^= 0x20; escaped = 0; }
        else if (c == 0x7d) { escaped = 1; continue; }
        if (received_len == sizeof(received)) { discard_frame = 1; continue; }
        received[received_len++] = c;
    }
}
static void service_guests(void)
{
    uint8_t bytes[2048];
    for (unsigned i = 0; i < VMD_MAX_GUESTS; i++) {
        struct guest *g = &guests[i];
        if (!g->name[0]) continue;
        /* Bound work per child, so continuous output cannot starve control. */
        for (unsigned batch = 0; batch < 8 && g->output_fd >= 0; batch++) {
            ssize_t n = read(g->output_fd, bytes, sizeof(bytes));
            if (n > 0) {
                for (ssize_t j = 0; j < n; j++)
                    g->history[g->output_end++ % VMD_HISTORY] = bytes[j];
            } else {
                if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
                    close(g->output_fd); g->output_fd = -1;
                }
                break;
            }
        }
        if (g->input_fd >= 0 && g->input_begin != g->input_end) {
            unsigned n = g->input_end > g->input_begin ? g->input_end - g->input_begin :
                         VMD_INPUT - g->input_begin;
            ssize_t wrote = write(g->input_fd, g->input + g->input_begin, n);
            if (wrote > 0) g->input_begin = (g->input_begin + (unsigned)wrote) % VMD_INPUT;
            else if (wrote < 0 && errno != EAGAIN && errno != EINTR) {
                close(g->input_fd); g->input_fd = -1;
            }
        }
        if (g->pid <= 0) {
            if (g->state == VMD_DRAINING && g->output_fd < 0) g->state = VMD_EXITED;
            continue;
        }
        int status;
        pid_t pid = waitpid(g->pid, &status, WNOHANG | WUNTRACED | WCONTINUED);
        if (pid > 0 && (WIFEXITED(status) || WIFSIGNALED(status))) {
            g->pid = 0; g->state = g->output_fd < 0 ? VMD_EXITED : VMD_DRAINING;
            g->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            if (g->input_fd >= 0) close(g->input_fd);
            g->input_fd = -1;
        } else if (pid > 0 && WIFSTOPPED(status) && g->state != VMD_STOPPING) {
            g->state = VMD_PAUSED;
        } else if (pid > 0 && WIFCONTINUED(status) && g->state != VMD_STOPPING) {
            g->state = VMD_RUNNING;
        }
        if (g->pid > 0 && g->state == VMD_STOPPING && now_ms() >= g->stop_deadline) {
            kill(g->pid, SIGKILL);
            g->stop_deadline = now_ms() + 2000;
        }
    }
}
/* Sleep until a control request or guest output arrives.  A plain nanosleep
 * depends on the timer wakeup path, which can be delayed by seconds while
 * guest vCPUs run; data arrival must be able to wake the supervisor on its
 * own, or START/LIST time out even though the request is already queued. */
static void wait_for_work(void)
{
    struct pollfd pfd[1 + VMD_MAX_GUESTS];
    unsigned n = 0;
    pfd[n].fd = control_fd;
    pfd[n].events = POLLIN;
    n++;
    for (unsigned i = 0; i < VMD_MAX_GUESTS; i++) {
        if (guests[i].name[0] && guests[i].output_fd >= 0) {
            pfd[n].fd = guests[i].output_fd;
            pfd[n].events = POLLIN;
            n++;
        }
    }
    if (poll(pfd, n, 5) < 0 && errno != EINTR) {
        struct timespec ts = {0, 5000000};
        nanosleep(&ts, NULL);
    }
}

int main(void)
{
    if (getuid() != 0) { fprintf(stderr, "a20-vmd requires root\n"); return 1; }
    struct timespec boot;
    if (clock_gettime(CLOCK_MONOTONIC, &boot) < 0) return 1;
    manager_epoch = ((uint64_t)boot.tv_sec * 1000000000 + boot.tv_nsec) ^
                    ((uint64_t)(unsigned)getpid() << 32);
    if (!manager_epoch) manager_epoch = 1;
    umask(0077);
    int lock = open("/tmp/a20-vmd.lock", O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0) {
        perror("a20-vmd: lock"); return 1;
    }
    for (unsigned i = 0; i < VMD_MAX_GUESTS; i++)
        guests[i].input_fd = guests[i].output_fd = -1;
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_signal); signal(SIGINT, on_signal);
    /* Drivers can be loaded asynchronously from the Runtime DriverStore. */
    uint64_t deadline = now_ms() + 30000;
    do {
        control_fd = open("/dev/vport0", O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (control_fd >= 0) break;
        struct timespec ts = {0, 20000000}; nanosleep(&ts, NULL);
    } while (!shutting_down && now_ms() < deadline);
    int32_t mirror = 0;
    if (control_fd < 0 || ioctl(control_fd, VPORT_IOCTL_SET_CONSOLE, &mirror) < 0) {
        perror("a20-vmd: /dev/vport0"); return 1;
    }
    printf("A20VMD: READY protocol=%u guests=%u memory_mb=%u\n",
           VMD_VERSION, VMD_MAX_GUESTS, VMD_MEMORY_MB);
    fflush(stdout);
    while (!shutting_down) {
        service_guests(); receive_requests();
        wait_for_work();
    }
    for (unsigned i = 0; i < VMD_MAX_GUESTS; i++) stop_guest(&guests[i]);
    for (;;) {
        unsigned alive = 0;
        service_guests();
        for (unsigned i = 0; i < VMD_MAX_GUESTS; i++) alive += guests[i].pid > 0;
        if (!alive) break;
        struct timespec ts = {0, 20000000}; nanosleep(&ts, NULL);
    }
    for (unsigned i = 0; i < VMD_MAX_GUESTS; i++) close_pipes(&guests[i]);
    close(control_fd); close(lock);
    return 0;
}
