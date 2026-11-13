/* SPDX-License-Identifier: BSD-3-Clause
 * Built as a separate userspace program with the upstream GPL Wi-Fi stack. */
#define _GNU_SOURCE
#include "control.h"
#include "rtw89_glue.h"
#include "uapi/a20/wifi.h"
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern int rtw89_selftest(void);
extern uintptr_t rtw88_kfree_min_addr;
extern int rtw88_log_level;
extern int a20_wifi_kmsg_fd;
extern void IOLog(const char *format, ...);

static int hardware = -1;
static volatile sig_atomic_t stopping;
static atomic_bool interrupt_polling, datapath_stopping, link_changed;
static pthread_t datapath;
static struct a20_wifi_info card;
static char interface[IF_NAMESIZE];
static pid_t dhcp_pid;
static uint8_t joined_ssid[32];
static uint32_t joined_ssid_length;
extern char **environ;

static void wifi_error(const char *operation)
{
    int error = errno;
    IOLog("[WIFI] %s: %s (errno=%d)\n", operation, strerror(error), error);
    (void)fsync(STDERR_FILENO);
    errno = error;
}

static void stop_signal(int signal) { (void)signal; stopping = 1; }

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static uint32_t config_read(void *context, unsigned offset, unsigned width)
{
    (void)context;
    struct a20_wifi_cfg cfg = {.offset = offset, .width = width};
    if (ioctl(hardware, A20_WIFI_CFG_READ, &cfg)) {
        wifi_error("PCI config read");
        return UINT32_MAX;
    }
    return cfg.value;
}

static void config_write(void *context, unsigned offset, unsigned width, uint32_t value)
{
    (void)context;
    struct a20_wifi_cfg cfg = {.offset = offset, .width = width, .value = value};
    if (ioctl(hardware, A20_WIFI_CFG_WRITE, &cfg))
        wifi_error("PCI config write");
}

struct allocation {
    struct a20_wifi_dma dma;
    void *memory;
};

static void *dma_allocate(void *context, size_t size, uint64_t *bus, void **cookie)
{
    (void)context;
    struct allocation *allocation = calloc(1, sizeof(*allocation));
    if (!allocation)
        return NULL;
    allocation->dma.size = size;
    if (ioctl(hardware, A20_WIFI_DMA_ALLOC, &allocation->dma)) {
        wifi_error("DMA allocation");
        free(allocation);
        return NULL;
    }
    allocation->memory = mmap(NULL, allocation->dma.size, PROT_READ | PROT_WRITE,
                              MAP_SHARED, hardware,
                              allocation->dma.token * A20_WIFI_DMA_OFFSET);
    if (allocation->memory == MAP_FAILED) {
        wifi_error("DMA mapping");
        (void)ioctl(hardware, A20_WIFI_DMA_FREE, &allocation->dma);
        free(allocation);
        return NULL;
    }
    *bus = allocation->dma.bus_address;
    *cookie = allocation;
    return allocation->memory;
}

static void dma_release(void *context, void *cookie)
{
    (void)context;
    struct allocation *allocation = cookie;
    /* DMA_FREE revokes the one user mapping before releasing its pages. */
    if (ioctl(hardware, A20_WIFI_DMA_FREE, &allocation->dma))
        wifi_error("DMA release");
    free(allocation);
}

static void irq_enable(void *context, bool enabled)
{
    (void)context;
    atomic_store(&interrupt_polling, enabled);
}
static void changed(void *context)
{
    (void)context;
    atomic_store(&link_changed, true);
}
static void received(void *context, const uint8_t *frame, size_t length)
{
    (void)context;
    if (write(hardware, frame, length) < 0 && errno != EAGAIN && errno != ENETDOWN)
        wifi_error("receive transport");
}

static void publish_link(void)
{
    struct rtw89_glue_link state;
    rtw89_glue_link(&state);
    struct a20_wifi_link link = {
        .authorized = state.state == RTW89_GLUE_LINK_CONNECTED && state.authorized,
    };
    if (ioctl(hardware, A20_WIFI_LINK, &link))
        wifi_error("carrier");
}

static void *pump(void *argument)
{
    (void)argument;
    uint8_t packet[2048];
    ssize_t pending = 0;
    while (!atomic_load(&datapath_stopping)) {
        if (atomic_load(&interrupt_polling))
            rtw89_glue_interrupt();
        if (atomic_exchange(&link_changed, false))
            publish_link();
        if (!pending) {
            ssize_t length = read(hardware, packet, sizeof(packet));
            if (length > 0)
                pending = length;
        }
        if (pending && rtw89_glue_tx_room()) {
            uint8_t *frame;
            void *handle = rtw89_glue_tx_alloc(pending, &frame);
            if (handle) {
                memcpy(frame, packet, pending);
                (void)rtw89_glue_tx(handle); /* consumes the handle on all paths */
                pending = 0;
            }
        }
        usleep(1000);
    }
    return NULL;
}

static void find_interface(void)
{
    int socket_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct if_nameindex *names = if_nameindex();
    if (socket_fd >= 0 && names) {
        for (struct if_nameindex *name = names; name->if_index; name++) {
            struct ifreq request = {0};
            snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", name->if_name);
            if (!ioctl(socket_fd, SIOCGIFHWADDR, &request) &&
                !memcmp(request.ifr_hwaddr.sa_data, card.mac, 6)) {
                snprintf(interface, sizeof(interface), "%s", name->if_name);
                break;
            }
        }
    }
    if (names)
        if_freenameindex(names);
    if (socket_fd >= 0)
        close(socket_fd);
}

static int address_text(char *out, size_t size)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct ifreq request = {0};
    snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", interface);
    int valid = fd >= 0 && !ioctl(fd, SIOCGIFADDR, &request) &&
                ((struct sockaddr_in *)&request.ifr_addr)->sin_addr.s_addr;
    if (valid)
        inet_ntop(AF_INET, &((struct sockaddr_in *)&request.ifr_addr)->sin_addr, out, size);
    else
        snprintf(out, size, "none");
    if (fd >= 0)
        close(fd);
    return valid;
}

static void stop_dhcp(void)
{
    if (dhcp_pid <= 0)
        return;
    kill(-dhcp_pid, SIGTERM);
    uint64_t deadline = monotonic_ms() + 2000;
    while (waitpid(dhcp_pid, NULL, WNOHANG) == 0 && monotonic_ms() < deadline)
        usleep(20000);
    if (waitpid(dhcp_pid, NULL, WNOHANG) == 0) {
        kill(-dhcp_pid, SIGKILL);
        while (waitpid(dhcp_pid, NULL, 0) < 0 && errno == EINTR) {}
    }
    dhcp_pid = 0;
    unlink("/run/a20-wifi-lease");
}

static int start_dhcp(void)
{
    stop_dhcp();
    unlink("/run/a20-wifi-lease");
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct ifreq request = {0};
    snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", interface);
    request.ifr_addr.sa_family = AF_INET;
    if (fd < 0 || ioctl(fd, SIOCSIFADDR, &request) ||
        ioctl(fd, SIOCGIFFLAGS, &request)) {
        int error = errno;
        if (fd >= 0)
            close(fd);
        return -error;
    }
    request.ifr_flags |= IFF_UP;
    int result = ioctl(fd, SIOCSIFFLAGS, &request);
    int error = errno;
    close(fd);
    if (result)
        return -error;
    const char *script = access("/a20/wifi-dhcp", X_OK) == 0 ?
                         "/a20/wifi-dhcp" : "/bin/wifi-dhcp";
    char *arguments[] = {"udhcpc", "-f", "-n", "-t", "5", "-O", "114", "-i", interface,
                         "-s", (char *)script, NULL};
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);
    error = posix_spawnp(&dhcp_pid, "udhcpc", NULL, &attr, arguments, environ);
    posix_spawnattr_destroy(&attr);
    if (error) {
        dhcp_pid = 0;
        return -error;
    }
    uint64_t deadline = monotonic_ms() + 20000;
    while (!stopping && monotonic_ms() < deadline) {
        char address[INET_ADDRSTRLEN];
        if (access("/run/a20-wifi-lease", F_OK) == 0 && address_text(address, sizeof(address))) {
            IOLog("[WIFI] DHCP lease ready on %s address=%s\n", interface, address);
            return 0;
        }
        if (waitpid(dhcp_pid, NULL, WNOHANG) == dhcp_pid) {
            dhcp_pid = 0;
            return -ENETUNREACH;
        }
        usleep(50000);
    }
    return -ETIMEDOUT;
}

static size_t status_text(char *buffer, size_t capacity)
{
    struct rtw89_glue_link link;
    rtw89_glue_link(&link);
    char address[INET_ADDRSTRLEN];
    (void)address_text(address, sizeof(address));
    return snprintf(buffer, capacity,
                    "interface=%s state=%u authorized=%u frequency=%u signal=%d error=%d "
                    "tx=%u rx=%u ip=%s\n",
                    interface, link.state, link.authorized, link.freq, link.signal,
                    link.last_error, link.tx_frames, link.rx_frames, address);
}

static int wait_scan(void)
{
    uint64_t deadline = monotonic_ms() + 20000;
    while (rtw89_glue_scanning() && !stopping) {
        if (monotonic_ms() >= deadline)
            return -ETIMEDOUT;
        usleep(20000);
    }
    return stopping ? -EINTR : 0;
}

static int connect_network(const struct wifi_command *command)
{
    struct rtw89_glue_link current;
    rtw89_glue_link(&current);
    if (current.state == RTW89_GLUE_LINK_CONNECTED && current.authorized &&
        joined_ssid_length == command->ssid_length &&
        !memcmp(joined_ssid, command->ssid, joined_ssid_length)) {
        char address[INET_ADDRSTRLEN];
        IOLog("[WIFI] already associated; preserving radio link\n");
        if (access("/run/a20-wifi-lease", F_OK) == 0 && address_text(address, sizeof(address)))
            return 0;
        return start_dhcp();
    }
    stop_dhcp();
    joined_ssid_length = 0;
    if (current.state != RTW89_GLUE_LINK_DOWN) {
        rtw89_glue_leave();
        publish_link();
    }
    IOLog("[WIFI] directed scan starting\n");
    int result = rtw89_glue_scan_for((const uint8_t *)command->ssid, command->ssid_length);
    if (!result)
        result = wait_scan();
    if (result) {
        IOLog("[WIFI] directed scan failed (%d)\n", result);
        return result;
    }
    IOLog("[WIFI] association/authentication starting\n");
    result = rtw89_glue_join((const uint8_t *)command->ssid, command->ssid_length,
                             command->password, command->password_length);
    if (result) {
        IOLog("[WIFI] association request failed (%d)\n", result);
        return result;
    }
    uint64_t deadline = monotonic_ms() + 45000;
    while (!stopping && monotonic_ms() < deadline) {
        struct rtw89_glue_link link;
        rtw89_glue_link(&link);
        if (link.state == RTW89_GLUE_LINK_CONNECTED && link.authorized) {
            memcpy(joined_ssid, command->ssid, command->ssid_length);
            joined_ssid_length = command->ssid_length;
            publish_link();
            IOLog("[WIFI] authenticated; DHCP starting on %s\n", interface);
            result = start_dhcp();
            IOLog("[WIFI] DHCP completed (%d)\n", result);
            return result;
        }
        if (link.state == RTW89_GLUE_LINK_DOWN && link.last_error) {
            IOLog("[WIFI] association/authentication failed (%d)\n", link.last_error);
            return link.last_error;
        }
        usleep(50000);
    }
    IOLog("[WIFI] association/authentication timed out\n");
    return -ETIMEDOUT;
}

static int read_full(int fd, void *buffer, size_t size)
{
    unsigned char *bytes = buffer;
    while (size) {
        ssize_t count = read(fd, bytes, size);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        bytes += count;
        size -= count;
    }
    return 0;
}
static int write_full(int fd, const void *buffer, size_t size)
{
    const unsigned char *bytes = buffer;
    while (size) {
        ssize_t count = write(fd, bytes, size);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        bytes += count;
        size -= count;
    }
    return 0;
}

static void handle_command(int fd)
{
    struct timeval timeout = {.tv_sec = 3};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    struct wifi_command command = {0};
    struct wifi_reply reply = {.result = -EINVAL};
    char text[20000] = {0};
    if (!read_full(fd, &command, sizeof(command)) &&
        command.version == WIFI_COMMAND_VERSION && command.ssid_length <= 32 &&
        command.password_length <= 63) {
        if (command.operation == WIFI_STATUS) {
            reply.result = 0;
            reply.length = status_text(text, sizeof(text));
        } else if (command.operation == WIFI_SCAN || command.operation == WIFI_SCAN_JSON) {
            IOLog("[WIFI] scan starting\n");
            reply.result = rtw89_glue_scan();
            if (!reply.result)
                reply.result = wait_scan();
            if (!reply.result) {
                struct rtw89_glue_bss networks[128];
                unsigned count = rtw89_glue_scan_results(networks, 128);
                IOLog("[WIFI] scan completed: %u result(s)\n", count);
                if (command.operation == WIFI_SCAN_JSON)
                    text[reply.length++] = '[';
                for (unsigned i = 0; i < count && reply.length + 150 < sizeof(text); i++) {
                    const struct rtw89_glue_bss *network = &networks[i];
                    char ssid[33];
                    unsigned length = network->ssid_len > 32 ? 32 : network->ssid_len;
                    if (command.operation == WIFI_SCAN_JSON) {
                        char hex[65];
                        static const char digits[] = "0123456789abcdef";
                        for (unsigned j = 0; j < length; j++) {
                            unsigned char value = network->ssid[j];
                            hex[j * 2] = digits[value >> 4];
                            hex[j * 2 + 1] = digits[value & 15];
                        }
                        hex[length * 2] = 0;
                        reply.length += snprintf(text + reply.length, sizeof(text) - reply.length,
                            "%s{\"ssid_hex\":\"%s\",\"signal\":%d,\"frequency\":%u,\"security\":%u}",
                            i ? "," : "", hex, network->signal, network->freq, network->security);
                        continue;
                    }
                    for (unsigned j = 0; j < length; j++) {
                        unsigned char value = network->ssid[j];
                        ssid[j] = value < 32 || value == 127 ? '?' : value;
                    }
                    ssid[length] = 0;
                    reply.length += snprintf(text + reply.length, sizeof(text) - reply.length,
                        "%3d dBm %4u MHz security=%02x %s\n",
                        network->signal, network->freq, network->security, ssid);
                }
                if (command.operation == WIFI_SCAN_JSON)
                    text[reply.length++] = ']';
            }
            if (reply.result)
                IOLog("[WIFI] scan failed (%d)\n", reply.result);
        } else if (command.operation == WIFI_CONNECT && command.ssid_length) {
            reply.result = connect_network(&command);
            reply.length = status_text(text, sizeof(text));
        } else if (command.operation == WIFI_DISCONNECT) {
            stop_dhcp();
            joined_ssid_length = 0;
            rtw89_glue_leave();
            publish_link();
            reply.result = 0;
        }
    }
    if (command.operation == WIFI_STATUS || command.operation == WIFI_CONNECT)
        IOLog("[WIFI] control op=%u result=%d status: %s\n", command.operation,
              reply.result, reply.length ? text : "unavailable");
    explicit_bzero(&command, sizeof(command));
    (void)write_full(fd, &reply, sizeof(reply));
    (void)write_full(fd, text, reply.length);
}

int main(int argc, char **argv)
{
    rtw88_kfree_min_addr = 0;
    rtw88_log_level = 2;
    if (argc == 2 && !strcmp(argv[1], "--self-test")) {
        int result = rtw89_selftest();
        printf("rtw89 self-test %s\n", result ? "FAIL" : "PASS");
        return result ? 1 : 0;
    }
    if (argc != 1) {
        fprintf(stderr, "usage: rtw89d [--self-test]\n");
        return 2;
    }
    signal(SIGINT, stop_signal);
    signal(SIGTERM, stop_signal);
    signal(SIGPIPE, SIG_IGN);
    a20_wifi_kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC | O_NONBLOCK);
    IOLog("[WIFI] service starting; opening PCI transport\n");
    hardware = open("/dev/rtw89", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (hardware < 0 || ioctl(hardware, A20_WIFI_INFO, &card) ||
        card.version != A20_WIFI_ABI) {
        wifi_error("open hardware transport / INFO");
        return 1;
    }
    IOLog("[WIFI] PCI %04x:%04x subsystem=%04x:%04x BAR2 bytes=%llu\n",
          card.vendor, card.device, card.subvendor, card.subdevice,
          (unsigned long long)card.mmio_size);
    void *mmio = mmap(NULL, card.mmio_size, PROT_READ | PROT_WRITE, MAP_SHARED, hardware, 0);
    if (mmio == MAP_FAILED) {
        wifi_error("map BAR2");
        close(hardware);
        return 1;
    }
    IOLog("[WIFI] BAR2 user mapping ready; probing radio\n");
    struct rtw89_glue_platform platform = {
        .cfg_read = config_read, .cfg_write = config_write,
        .dma_alloc = dma_allocate, .dma_free = dma_release,
        .irq_enable = irq_enable, .link_changed = changed, .rx_frame = received,
    };
    struct rtw89_glue_device device = {
        .vendor = card.vendor, .device = card.device, .revision = card.revision,
        .subsystem_vendor = card.subvendor, .subsystem_device = card.subdevice,
        .mmio_base = mmio, .mmio_len = card.mmio_size,
    };
    int thread_error = pthread_create(&datapath, NULL, pump, NULL);
    int result = thread_error ? -thread_error : rtw89_glue_probe(&platform, &device);
    IOLog("[WIFI] radio probe result=%d\n", result);
    if (!result) {
        struct rtw89_glue_info radio;
        struct a20_wifi_hwaddr address = {0};
        if (!rtw89_glue_get_info(&radio))
            result = -ENODEV;
        else {
            memcpy(address.mac, radio.mac, 6);
            if (ioctl(hardware, A20_WIFI_HWADDR, &address))
                result = -errno;
            else
                memcpy(card.mac, radio.mac, 6);
        }
        IOLog("[WIFI] permanent MAC publication result=%d\n", result);
    }
    if (!result) {
        result = rtw89_glue_up();
        IOLog("[WIFI] radio start result=%d\n", result);
    }
    int listener = -1;
    if (!result) {
        find_interface();
        if (!interface[0])
            result = -ENODEV;
    }
    if (!result) {
        umask(0077);
        mkdir("/run", 0755);
        listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        struct sockaddr_un address = {.sun_family = AF_UNIX};
        snprintf(address.sun_path, sizeof(address.sun_path), "%s", WIFI_SOCKET);
        unlink(WIFI_SOCKET);
        if (listener < 0 || bind(listener, (struct sockaddr *)&address, sizeof(address)) ||
            listen(listener, 4))
            result = -errno;
    }
    if (!result) {
        IOLog("[WIFI] rtw89 radio ready on %s; control %s\n", interface, WIFI_SOCKET);
        while (!stopping) {
            if (dhcp_pid > 0) {
                struct rtw89_glue_link link;
                rtw89_glue_link(&link);
                if (link.state != RTW89_GLUE_LINK_CONNECTED || !link.authorized)
                    stop_dhcp();
            }
            struct pollfd descriptor = {.fd = listener, .events = POLLIN};
            if (poll(&descriptor, 1, 200) <= 0)
                continue;
            int client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
            if (client >= 0) {
                handle_command(client);
                close(client);
            }
        }
    } else {
        IOLog("[WIFI] bring-up failed (%d); see the preceding rtw89 diagnostic\n", result);
    }
    if (listener >= 0) {
        close(listener);
        unlink(WIFI_SOCKET);
    }
    /* Keep interrupt processing alive until the radio has drained its work. */
    stop_dhcp();
    rtw89_glue_down();
    atomic_store(&datapath_stopping, true);
    if (!thread_error)
        pthread_join(datapath, NULL);
    rtw89_glue_remove();
    (void)ioctl(hardware, A20_WIFI_QUIESCE, NULL);
    close(hardware);
    return result ? 1 : 0;
}
