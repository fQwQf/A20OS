#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define TEST_FILE "/tmp/io_event_test.txt"
#define TEST_MSG  "io_event_block_data"
#define TEST_PORT 12347

static int test_block_io(void)
{
    int fd = open(TEST_FILE, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) {
        printf("IO_EVENT_TEST: FAIL block open errno=%d\n", errno);
        return -1;
    }

    if (write(fd, TEST_MSG, strlen(TEST_MSG)) != (ssize_t)strlen(TEST_MSG)) {
        printf("IO_EVENT_TEST: FAIL block write errno=%d\n", errno);
        close(fd);
        return -1;
    }

    if (lseek(fd, 0, SEEK_SET) < 0) {
        printf("IO_EVENT_TEST: FAIL block lseek errno=%d\n", errno);
        close(fd);
        return -1;
    }

    char buf[64] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n != (ssize_t)strlen(TEST_MSG) || strcmp(buf, TEST_MSG) != 0) {
        printf("IO_EVENT_TEST: FAIL block read mismatch n=%zd buf=%s\n", n, buf);
        close(fd);
        return -1;
    }

    close(fd);
    unlink(TEST_FILE);
    return 0;
}

static int test_udp_loopback(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        printf("IO_EVENT_TEST: FAIL udp socket errno=%d\n", errno);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(TEST_PORT);

    const char *msg = "io_event_udp";
    if (sendto(fd, msg, strlen(msg), 0,
               (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("IO_EVENT_TEST: FAIL udp sendto errno=%d\n", errno);
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

int main(void)
{
    if (test_block_io() < 0)
        return 1;
    if (test_udp_loopback() < 0)
        return 1;

    printf("IO_EVENT_TEST: PASS\n");
    return 0;
}
