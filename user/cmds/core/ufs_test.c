/*
 * ufs_test — 用户态文件系统服务（ufsd）端到端验证。
 *
 * 前置：QEMU 已挂第三块 virtio-blk 盘（DEV_CLASS_BLOCK 序号 2，FAT32，
 * 内含 HELLO.TXT）。流程：
 *   1. fork/exec /bin/ufsd-rv，让它把块设备 2 以 uxfs 形态挂到 /ufs；
 *   2. 轮询 /ufs/HELLO.TXT 可见（挂载 + INIT 握手完成）；
 *   3. 读回 HELLO.TXT 内容并比对；
 *   4. 新建 ROUND.BIN 写入校验图案、读回比对；
 *   5. 列目录应含两个文件；
 *   6. unlink ROUND.BIN 后确认消失。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>

static int fail(const char *why)
{
    printf("UXFS_FS: FAIL %s\n", why);
    return 1;
}

static void spawn_ufsd(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/ufsd-rv", "ufsd-rv", "/ufs", "1", (char *)0);
        _exit(90);
    }
    (void)pid; /* 服务常驻；父进程继续轮询挂载点 */
}

int main(void)
{
    const char *hello_path = "/ufs/HELLO.TXT";
    const char *expect = "hello-uxfs\n";

    /* 1-2. 拉起服务并等待挂载可见。 */
    int mounted = 0;
    struct stat st;
    if (stat(hello_path, &st) == 0) {
        mounted = 1;
    } else {
        spawn_ufsd();
        for (int i = 0; i < 500; i++) {
            if (stat(hello_path, &st) == 0) { mounted = 1; break; }
            usleep(20000);
        }
    }
    if (!mounted)
        return fail("/ufs not visible after ufsd spawn");
    printf("UXFS_FS: /ufs mounted, HELLO.TXT present\n");

    /* 3. 读回预置内容。 */
    char buf[64];
    memset(buf, 0, sizeof(buf));
    int fd = open(hello_path, O_RDONLY);
    if (fd < 0)
        return fail("open HELLO.TXT");
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0 || strcmp(buf, expect) != 0)
        return fail("HELLO.TXT content mismatch");
    printf("UXFS_FS: read-back ok (%zd bytes)\n", n);

    /* 4. 新建文件写入图案并读回。 */
    const char *round_path = "/ufs/ROUND.BIN";
    static unsigned char pattern[8192];
    for (size_t i = 0; i < sizeof(pattern); i++)
        pattern[i] = (unsigned char)(i * 31 + 7);
    fd = open(round_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0)
        return fail("create ROUND.BIN");
    ssize_t wn = write(fd, pattern, sizeof(pattern));
    close(fd);
    if (wn != (ssize_t)sizeof(pattern))
        return fail("write ROUND.BIN");

    static unsigned char back[8192];
    fd = open(round_path, O_RDONLY);
    if (fd < 0)
        return fail("reopen ROUND.BIN");
    ssize_t rn = read(fd, back, sizeof(back));
    close(fd);
    if (rn != (ssize_t)sizeof(pattern) || memcmp(pattern, back, sizeof(pattern)) != 0)
        return fail("read-back mismatch on ROUND.BIN");
    printf("UXFS_FS: create/write/read ok (%zd bytes)\n", rn);

    /* 5. 目录列举。 */
    DIR *d = opendir("/ufs");
    if (!d)
        return fail("opendir /ufs");
    int saw_hello = 0, saw_round = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, "HELLO.TXT") == 0) saw_hello = 1;
        if (strcmp(ent->d_name, "ROUND.BIN") == 0) saw_round = 1;
    }
    closedir(d);
    if (!saw_hello || !saw_round)
        return fail("readdir missing entries");
    printf("UXFS_FS: readdir ok\n");

    /* 6. 删除后确认消失。 */
    if (unlink(round_path) != 0)
        return fail("unlink ROUND.BIN");
    if (stat(round_path, &st) == 0)
        return fail("unlink did not take effect");
    printf("UXFS_FS: PASS\n");
    return 0;
}
