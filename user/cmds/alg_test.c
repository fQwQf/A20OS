#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef AF_ALG
#define AF_ALG 38
#endif

struct sockaddr_alg {
    uint16_t salg_family;
    uint8_t  salg_type[14];
    uint32_t salg_feat;
    uint32_t salg_mask;
    uint8_t  salg_name[64];
};

int main(void) {
    int fd = socket(AF_ALG, SOCK_SEQPACKET, 0);
    if (fd < 0) {
        if (errno == EAFNOSUPPORT || errno == EPROTONOSUPPORT) {
            printf("ALG_TEST: SKIP\n");
            return 77;
        }
        perror("socket AF_ALG");
        printf("ALG_TEST: FAIL\n");
        return 1;
    }

    struct sockaddr_alg sa = {
        .salg_type = "hash",
        .salg_name = "sha256",
    };

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        if (errno == EAFNOSUPPORT || errno == ENOENT) {
            close(fd);
            printf("ALG_TEST: SKIP\n");
            return 77;
        }
        perror("bind AF_ALG");
        close(fd);
        printf("ALG_TEST: FAIL\n");
        return 1;
    }

    int accept_fd = accept(fd, NULL, NULL);
    if (accept_fd < 0) {
        perror("accept AF_ALG");
        close(fd);
        printf("ALG_TEST: FAIL\n");
        return 1;
    }

    const char *msg = "abc";
    if (send(accept_fd, msg, strlen(msg), 0) < 0) {
        perror("send AF_ALG");
        close(accept_fd);
        close(fd);
        printf("ALG_TEST: FAIL\n");
        return 1;
    }

    uint8_t digest[32];
    ssize_t n = recv(accept_fd, digest, sizeof(digest), 0);
    close(accept_fd);
    close(fd);

    if (n == (ssize_t)sizeof(digest)) {
        printf("ALG_TEST: PASS\n");
        return 0;
    }
    printf("ALG_TEST: FAIL\n");
    return 1;
}
