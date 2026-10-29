/* Real BSD UDP/TCP loopback through its socket syscalls, IP input/output and
 * deferred netisr queue. This file is compiled with BSD's private headers. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/socket.h>
#include <sys/errno.h>
#include <netinet/in.h>
#include "ff_api.h"
#include "ff_host_interface.h"
extern int port_last_error(void);
struct linux_inet_address {
    uint16_t family, port;
    uint32_t address;
    uint8_t reserved[8];
};
static void progress(void) { ff_swi_net_excute(); }
static int again(void) { return port_last_error() == EWOULDBLOCK || port_last_error() == EINTR; }
static int receive_exact(int fd, uint8_t *buffer, size_t length)
{
    size_t done = 0;
    for (unsigned i = 0; i < 256 && done < length; ++i) {
        progress();
        ssize_t got = ff_read(fd, buffer + done, length - done);
        if (got > 0) done += (size_t)got;
        else if (got == 0 || !again()) return EIO;
    }
    return done == length ? 0 : ETIMEDOUT;
}
int port_loopback_probe(void)
{
    struct linux_inet_address address = {AF_INET, htons(49271), htonl(INADDR_LOOPBACK), {0}};
    uint8_t payload[64], received[64];
    for (unsigned i = 0; i < sizeof(payload); ++i) payload[i] = (uint8_t)(i * 31 + 7);
    int result = 0, receiver = -1, sender = -1, listener = -1, client = -1, accepted = -1;
    /* 0x800 is Linux SOCK_NONBLOCK, translated by the real ff_socket wrapper. */
    receiver = ff_socket(AF_INET, SOCK_DGRAM | 0x800, 0);
    sender = ff_socket(AF_INET, SOCK_DGRAM | 0x800, 0);
    if (receiver < 0 || sender < 0) { result = port_last_error(); goto out; }
    if (ff_bind(receiver, (const struct linux_sockaddr *)&address, sizeof(address))) {
        result = port_last_error(); goto out;
    }
    if (ff_sendto(sender, payload, sizeof(payload), 0,
                  (const struct linux_sockaddr *)&address, sizeof(address)) != sizeof(payload)) {
        result = port_last_error(); goto out;
    }
    result = receive_exact(receiver, received, sizeof(received));
    if (result || memcmp(payload, received, sizeof(payload))) { result = EIO; goto out; }
    printf("BSD_UDP_LOOPBACK_PASS\n");

    address.port = htons(49272);
    listener = ff_socket(AF_INET, SOCK_STREAM | 0x800, 0);
    client = ff_socket(AF_INET, SOCK_STREAM | 0x800, 0);
    if (listener < 0 || client < 0) { result = port_last_error(); goto out; }
    if (ff_bind(listener, (const struct linux_sockaddr *)&address, sizeof(address)) || ff_listen(listener, 4)) {
        result = port_last_error(); goto out;
    }
    if (ff_connect(client, (const struct linux_sockaddr *)&address, sizeof(address)) && port_last_error() != EINPROGRESS) {
        result = port_last_error(); goto out;
    }
    for (unsigned i = 0; i < 256 && accepted < 0; ++i) {
        progress();
        accepted = ff_accept(listener, NULL, NULL);
        if (accepted < 0 && !again()) { result = port_last_error(); goto out; }
    }
    if (accepted < 0) { result = ETIMEDOUT; goto out; }
    /* Linux FIONBIO; accepted descriptors do not inherit nonblocking state. */
    int nonblocking = 1;
    if (ff_ioctl(accepted, 0x5421, &nonblocking)) { result = port_last_error(); goto out; }
    if (ff_write(client, payload, sizeof(payload)) != sizeof(payload)) { result = EIO; goto out; }
    result = receive_exact(accepted, received, sizeof(received));
    if (result || memcmp(payload, received, sizeof(payload))) { result = EIO; goto out; }
    if (ff_write(accepted, received, sizeof(received)) != sizeof(received)) { result = EIO; goto out; }
    memset(received, 0, sizeof(received));
    result = receive_exact(client, received, sizeof(received));
    if (result || memcmp(payload, received, sizeof(payload))) { result = EIO; goto out; }
    printf("BSD_TCP_LOOPBACK_PASS\n");
out:
    if (accepted >= 0) ff_close(accepted);
    if (client >= 0) ff_close(client);
    if (listener >= 0) ff_close(listener);
    if (sender >= 0) ff_close(sender);
    if (receiver >= 0) ff_close(receiver);
    progress();
    return result;
}
