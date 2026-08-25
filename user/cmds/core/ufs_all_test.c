/*
 * ufs_all_test — 用户态文件系统宿主（ufsd）全后端端到端验证。
 *
 * QEMU 布局（DEV_CLASS_BLOCK 序号）：
 *   0  bus.0 主存储 fat32.img（内核引导用）
 *   1  bus.2 fat scratch（既有 UFS_SCRATCH_IMG，含 HELLO.TXT）
 *   2  bus.4 ext4 scratch（mke2fs -d 预置 hello.txt）
 *   3  bus.6 iso9660（mkisofs_test 生成，含 HELLO.TXT 与 SUB/NESTED.TXT）
 *   4  bus.7 ntfs scratch（mkntfs 快速格式化，空卷）
 *
 * 每个后端拉起独立 ufsd 实例并执行 POSIX 操作序列：
 *   fat : 读回 / 写入读回 / 列目录 / 删除
 *   ext4: 预置内容读回 / 新建写读 / rename / 删除
 *   iso : 只读内容校验 / 列目录 / 嵌套路径读取
 *   ntfs: 空卷挂载 / 新建写读 / 列目录 / 删除
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/mount.h>

static int fail(const char *tag, const char *why)
{
    printf("%s: FAIL %s\n", tag, why);
    return 1;
}

static pid_t spawn_ufsd(const char *mp, const char *idx, const char *fstype)
{
    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/ufsd-rv", "ufsd-rv", mp, idx, fstype, (char *)0);
        _exit(90);
    }
    return pid;
}

static int wait_path(const char *path, int tries)
{
    struct stat st;
    for (int i = 0; i < tries; i++) {
        if (stat(path, &st) == 0)
            return 0;
        usleep(20000);
    }
    return -1;
}

static int read_all(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    return (int)n;
}

static unsigned char pattern[8192];

static int roundtrip(const char *tag, const char *dir, const char *name)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", dir, name);

    for (size_t i = 0; i < sizeof(pattern); i++)
        pattern[i] = (unsigned char)(i * 31 + 7);

    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0)
        return fail(tag, "create");
    ssize_t wn = write(fd, pattern, sizeof(pattern));
    close(fd);
    if (wn != (ssize_t)sizeof(pattern))
        return fail(tag, "write size");

    static unsigned char back[8192];
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return fail(tag, "reopen");
    ssize_t rn = read(fd, back, sizeof(back));
    close(fd);
    if (rn != (ssize_t)sizeof(pattern) ||
        memcmp(pattern, back, sizeof(pattern)) != 0)
        return fail(tag, "read-back mismatch");
    return 0;
}

static int test_fat(void)
{
    const char *tag = "UXFS_FAT";
    struct stat st0;
    if (stat("/ufs/HELLO.TXT", &st0) != 0)
        spawn_ufsd("/ufs", "1", "fat"); /* svcmgr 未托管时兜底自拉起 */
    if (wait_path("/ufs/HELLO.TXT", 500))
        return fail(tag, "/ufs not visible");

    char buf[64];
    if (read_all("/ufs/HELLO.TXT", buf, sizeof(buf)) <= 0 ||
        strcmp(buf, "hello-uxfs\n") != 0)
        return fail(tag, "content mismatch");

    if (roundtrip(tag, "/ufs", "ROUND.BIN"))
        return 1;
    if (unlink("/ufs/ROUND.BIN") != 0)
        return fail(tag, "unlink");

    printf("%s: PASS\n", tag);
    return 0;
}

static int test_ext4(void)
{
    const char *tag = "UXFS_EXT4";
    spawn_ufsd("/uext4", "2", "ext4");
    if (wait_path("/uext4/hello.txt", 500))
        return fail(tag, "/uext4 not visible");

    char buf[64];
    if (read_all("/uext4/hello.txt", buf, sizeof(buf)) <= 0 ||
        strcmp(buf, "hello ext4 user-space\n") != 0)
        return fail(tag, "seeded content mismatch");

    if (roundtrip(tag, "/uext4", "EXT4BIN.BIN"))
        return 1;

    /* rename 语义（ext4 支持完整 rename 原语） */
    if (rename("/uext4/EXT4BIN.BIN", "/uext4/RENAMED.BIN") != 0)
        return fail(tag, "rename");
    struct stat st;
    if (stat("/uext4/EXT4BIN.BIN", &st) == 0 || stat("/uext4/RENAMED.BIN", &st) != 0)
        return fail(tag, "rename effect");

    if (unlink("/uext4/RENAMED.BIN") != 0)
        return fail(tag, "unlink");

    printf("%s: PASS\n", tag);
    return 0;
}

static int test_iso(void)
{
    const char *tag = "UXFS_ISO";
    spawn_ufsd("/uiso", "3", "iso9660");
    if (wait_path("/uiso/hello.txt", 500))
        return fail(tag, "/uiso not visible");

    char buf[64];
    if (read_all("/uiso/hello.txt", buf, sizeof(buf)) <= 0 ||
        strcmp(buf, "hello iso9660 world\n") != 0)
        return fail(tag, "HELLO.TXT content");

    if (read_all("/uiso/sub/nested.txt", buf, sizeof(buf)) <= 0 ||
        strcmp(buf, "nested file content\n") != 0)
        return fail(tag, "NESTED.TXT content");

    DIR *d = opendir("/uiso");
    if (!d)
        return fail(tag, "opendir");
    int saw_sub = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL)
        if (strcmp(ent->d_name, "sub") == 0)
            saw_sub = 1;
    closedir(d);
    if (!saw_sub)
        return fail(tag, "readdir missing SUB");

    printf("%s: PASS\n", tag);
    return 0;
}

/* ntfs 后端 v1 为只读放置（内核 ntfs 写路径无在库测试覆盖）：
 * 验证挂载/引导扇区+MFT 解析/目录枚举/只读语义。 */
static int test_ntfs(void)
{
    const char *tag = "UXFS_NTFS";
    spawn_ufsd("/untfs", "4", "ntfs");
    if (wait_path("/untfs", 500))
        return fail(tag, "/untfs not visible");

    struct stat st;
    if (stat("/untfs", &st) != 0)
        return fail(tag, "stat /untfs failed");
    if (!S_ISDIR(st.st_mode)) {
        printf("UXFS_NTFS: dbg mode=%o size=%zu\n", st.st_mode,
               (size_t)st.st_size);
        return fail(tag, "root not a directory");
    }

    DIR *d = opendir("/untfs");
    if (!d)
        return fail(tag, "opendir");
    /* 空卷合法；枚举调用链本身必须无错完成 */
    while (readdir(d) != NULL)
        ;
    closedir(d);

    /* 只读语义：写必须失败 */
    int fd = open("/untfs/NOPE.BIN", O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) {
        close(fd);
        return fail(tag, "write unexpectedly succeeded");
    }

    printf("%s: PASS (ro)\n", tag);
    return 0;
}

/* 崩溃恢复演练：独立挂载点 /ufs2 上验证"杀实例 -> 卸载 -> 重启 ->
 * 数据持久"的完整契约（06-user-fs.md 的恢复语义实测）。 */
static int test_restart(void)
{
    const char *tag = "UXFS_RESTART";
    const char *mrk = "/ufs2/RESTART.MRK";

    pid_t a = spawn_ufsd("/ufs2", "1", "fat");
    if (wait_path(mrk ? "/ufs2" : mrk, 500))
        return fail(tag, "/ufs2 first instance not visible");

    int fd = open(mrk, O_CREAT | O_WRONLY, 0644);
    if (fd < 0)
        return fail(tag, "create marker");
    if (write(fd, "survive-restart\n", 16) != 16) {
        close(fd);
        return fail(tag, "marker write");
    }
    close(fd);

    /* 模拟崩溃：SIGKILL 实例 A 并回收 */
    kill(a, SIGKILL);
    waitpid(a, NULL, 0);

    /* 内核侧挂载仍指向已死通道：先卸载才能重新注册 */
    if (umount2("/ufs2", 0) != 0)
        return fail(tag, "umount after crash");

    pid_t b = spawn_ufsd("/ufs2", "1", "fat");
    (void)b;
    if (wait_path(mrk, 500))
        return fail(tag, "instance B not visible");

    char buf[32];
    memset(buf, 0, sizeof(buf));
    int fd2 = open(mrk, O_RDONLY);
    if (fd2 < 0)
        return fail(tag, "marker reopen");
    ssize_t n = read(fd2, buf, sizeof(buf) - 1);
    close(fd2);
    if (n != 16 || strcmp(buf, "survive-restart\n") != 0)
        return fail(tag, "marker content lost across restart");

    unlink(mrk);
    printf("%s: PASS\n", tag);
    return 0;
}

int main(void)
{
    if (test_fat())
        return 1;
    if (test_ext4())
        return 1;
    if (test_iso())
        return 1;
    if (test_ntfs())
        return 1;
    if (test_restart())
        return 1;
    printf("UXFS_ALL: PASS\n");
    return 0;
}
