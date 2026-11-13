/* SPDX-License-Identifier: BSD-3-Clause */
#define _GNU_SOURCE
#include "control.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int transfer(int fd, void *data, size_t size, int sending)
{
    char *bytes = data;
    while (size) {
        ssize_t count = sending ? write(fd, bytes, size) : read(fd, bytes, size);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        bytes += count;
        size -= count;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct wifi_command command = {.version = WIFI_COMMAND_VERSION};
    if (argc == 2 && !strcmp(argv[1], "scan"))
        command.operation = WIFI_SCAN;
    else if (argc == 2 && !strcmp(argv[1], "status"))
        command.operation = WIFI_STATUS;
    else if (argc == 2 && !strcmp(argv[1], "disconnect"))
        command.operation = WIFI_DISCONNECT;
    else if (argc == 3 && !strcmp(argv[1], "connect") &&
             strlen(argv[2]) > 0 && strlen(argv[2]) <= sizeof(command.ssid)) {
        command.operation = WIFI_CONNECT;
        command.ssid_length = strlen(argv[2]);
        memcpy(command.ssid, argv[2], command.ssid_length);
        char *password = getpass("Wi-Fi password (Enter for an open network): ");
        if (!password || strlen(password) > 63) {
            fprintf(stderr, "Wi-Fi password must contain at most 63 bytes\n");
            return 2;
        }
        command.password_length = strlen(password);
        memcpy(command.password, password, command.password_length);
        explicit_bzero(password, strlen(password));
    } else {
        fprintf(stderr, "usage: wifi scan | status | connect SSID | disconnect\n");
        return 2;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", WIFI_SOCKET);
    if (fd < 0 || connect(fd, (struct sockaddr *)&address, sizeof(address))) {
        perror("wifi: wireless service");
        fprintf(stderr, "See /var/log/a20/wifi.log for radio bring-up diagnostics\n");
        explicit_bzero(&command, sizeof(command));
        return 1;
    }
    int error = transfer(fd, &command, sizeof(command), 1);
    explicit_bzero(&command, sizeof(command));
    struct wifi_reply reply;
    if (error || transfer(fd, &reply, sizeof(reply), 0) || reply.length > 20000) {
        fprintf(stderr, "wifi: invalid service response\n");
        close(fd);
        return 1;
    }
    char text[20001];
    if (transfer(fd, text, reply.length, 0)) {
        fprintf(stderr, "wifi: incomplete response\n");
        close(fd);
        return 1;
    }
    text[reply.length] = 0;
    fputs(text, stdout);
    close(fd);
    if (reply.result) {
        fprintf(stderr, "wifi: operation failed (%d)\n", reply.result);
        return 1;
    }
    return 0;
}
