#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { printf("DRM_PRIME: FAIL line=%d errno=%d\n", __LINE__, errno); return 1; } } while (0)
struct dumb { uint32_t height, width, bpp, flags, handle, pitch; uint64_t size; };
struct map { uint32_t handle, pad; uint64_t offset; };
struct prime { uint32_t handle, flags; int32_t fd; };
struct fb { uint32_t id, width, height, pitch, bpp, depth, handle; };

/* Independent opens share storage only after explicit import. Normal close
 * releases handles and framebuffers; an existing VMA owns its VMO separately. */
static int file_ownership(void)
{
    int a = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    int b = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    CHECK(a >= 0 && b >= 0);
    struct dumb d = {32, 32, 32, 0, 0, 0, 0};
    CHECK(ioctl(a, 0xc02064b2UL, &d) == 0);
    struct map m = {d.handle, 0, 0};
    CHECK(ioctl(a, 0xc01064b3UL, &m) == 0);
    uint32_t *pixels = mmap(NULL, d.size, PROT_READ | PROT_WRITE, MAP_SHARED, a, m.offset);
    CHECK(pixels != MAP_FAILED);
    pixels[0] = 0xa20f11e;
    errno = 0;
    CHECK(ioctl(b, 0xc01064b3UL, &m) == -1 && errno == ENOENT);
    errno = 0;
    CHECK(mmap(NULL, d.size, PROT_READ, MAP_SHARED, b, m.offset) == MAP_FAILED && errno == ENOENT);
    struct { uint32_t handle, pad; } close_gem = {d.handle, 0};
    errno = 0;
    CHECK(ioctl(b, 0x40086409UL, &close_gem) == -1 && errno == ENOENT);
    struct fb f = {0, d.width, d.height, d.pitch, 32, 24, d.handle};
    CHECK(ioctl(a, 0xc01c64aeUL, &f) == 0);
    struct fb info = {.id = f.id, .handle = 99};
    CHECK(ioctl(b, 0xc01c64adUL, &info) == 0 && info.handle == 0 && info.width == d.width);
    errno = 0;
    CHECK(ioctl(b, 0xc00464afUL, &f.id) == -1 && errno == ENOENT);
    struct prime p = {d.handle, O_RDWR | O_CLOEXEC, -1};
    CHECK(ioctl(a, 0xc00c642dUL, &p) == 0);
    struct prime imported = {0, 0, p.fd};
    CHECK(ioctl(b, 0xc00c642eUL, &imported) == 0 && imported.handle == d.handle);
    CHECK(ioctl(b, 0xc00c642eUL, &imported) == 0);
    CHECK(close(a) == 0 && close(p.fd) == 0); /* no explicit GEM_CLOSE/RMFB */
    info.id = f.id;
    errno = 0;
    CHECK(ioctl(b, 0xc01c64adUL, &info) == -1 && errno == ENOENT);
    CHECK(ioctl(b, 0xc01064b3UL, &m) == 0);
    uint32_t *shared = mmap(NULL, d.size, PROT_READ | PROT_WRITE, MAP_SHARED, b, m.offset);
    CHECK(shared != MAP_FAILED && shared[0] == pixels[0]);
    CHECK(close(b) == 0);
    shared[1] = 0xbac001;
    CHECK(pixels[1] == shared[1]);
    CHECK(munmap(shared, d.size) == 0 && munmap(pixels, d.size) == 0);
    puts("DRM_FILE: PASS ownership+close+framebuffer+prime+retained-vma");
    return 0;
}

static int transfer_fd(int sock, int fd)
{
    char c = 'F', control[CMSG_SPACE(sizeof(int))];
    struct iovec iov = {&c, 1};
    struct msghdr msg = {0};
    memset(control, 0, sizeof(control));
    msg.msg_iov = &iov; msg.msg_iovlen = 1;
    msg.msg_control = control; msg.msg_controllen = sizeof(control);
    struct cmsghdr *h = CMSG_FIRSTHDR(&msg);
    h->cmsg_level = SOL_SOCKET; h->cmsg_type = SCM_RIGHTS;
    h->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(h), &fd, sizeof(fd));
    return sendmsg(sock, &msg, 0) == 1 ? 0 : -1;
}

static int consume(int sock, size_t size)
{
    char c, control[CMSG_SPACE(sizeof(int))];
    struct iovec iov = {&c, 1};
    struct msghdr msg = {0};
    msg.msg_iov = &iov; msg.msg_iovlen = 1;
    msg.msg_control = control; msg.msg_controllen = sizeof(control);
    CHECK(recvmsg(sock, &msg, MSG_CMSG_CLOEXEC) == 1);
    struct cmsghdr *h = CMSG_FIRSTHDR(&msg);
    CHECK(h && h->cmsg_level == SOL_SOCKET && h->cmsg_type == SCM_RIGHTS);
    int fd; memcpy(&fd, CMSG_DATA(h), sizeof(fd));
    CHECK(read(sock, &c, 1) == 1); /* exporter has closed its handle and fd */
    int drm = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    CHECK(drm >= 0);
    struct prime imported = {0, 0, fd};
    CHECK(ioctl(drm, 0xc00c642eUL, &imported) == 0);
    struct map m = {imported.handle, 0, 0};
    CHECK(ioctl(drm, 0xc01064b3UL, &m) == 0);
    uint32_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, drm, m.offset);
    CHECK(pixels != MAP_FAILED && pixels[0] == 0x1234abcdu);
    uint64_t sync = 3; /* START READ|WRITE */
    CHECK(ioctl(fd, 0x40086200UL, &sync) == 0);
    pixels[1] = 0x87654321u;
    sync |= 4;
    CHECK(ioctl(fd, 0x40086200UL, &sync) == 0);
    struct { uint32_t handle, pad; } close_gem = {imported.handle, 0};
    CHECK(ioctl(drm, 0x40086409UL, &close_gem) == 0);
    CHECK(close(fd) == 0 && close(drm) == 0);
    CHECK(pixels[1] == 0x87654321u); /* mapping retains the shared VMO */
    CHECK(munmap(pixels, size) == 0);
    return 0;
}

int main(void)
{
    CHECK(file_ownership() == 0);
    int drm = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    CHECK(drm >= 0);
    struct dumb b = {64, 64, 32, 0, 0, 0, 0};
    CHECK(ioctl(drm, 0xc02064b2UL, &b) == 0);
    struct map m = {b.handle, 0, 0};
    CHECK(ioctl(drm, 0xc01064b3UL, &m) == 0);
    uint32_t *original = mmap(NULL, b.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm, m.offset);
    CHECK(original != MAP_FAILED);
    struct prime p = {b.handle, O_CLOEXEC | O_RDWR, -1};
    CHECK(ioctl(drm, 0xc00c642dUL, &p) == 0);
    CHECK(fcntl(p.fd, F_GETFD) & FD_CLOEXEC);
    struct stat st;
    CHECK(fstat(p.fd, &st) == 0 && (uint64_t)st.st_size == b.size);
    CHECK(lseek(p.fd, 0, SEEK_END) == (off_t)b.size);
    uint32_t *shared = mmap(NULL, b.size, PROT_READ | PROT_WRITE, MAP_SHARED, p.fd, 0);
    CHECK(shared != MAP_FAILED);
    original[0] = 0x1234abcdu;
    CHECK(shared[0] == original[0]);
    shared[2] = 0xfeedfaceu;
    CHECK(original[2] == 0xfeedfaceu);
    uint64_t bad_sync = 0;
    errno = 0;
    CHECK(ioctl(p.fd, 0x40086200UL, &bad_sync) == -1 && errno == EINVAL);
    struct { uint32_t flags; int32_t fd; } fence = {3, -1};
    CHECK(ioctl(p.fd, 0xc0086202UL, &fence) == 0 && fence.fd >= 0);
    struct pollfd ready = {fence.fd, POLLIN, 0};
    CHECK(poll(&ready, 1, 0) == 1 && (ready.revents & POLLIN));
    CHECK(ioctl(p.fd, 0x40086203UL, &fence) == 0);
    struct { char name[32]; int32_t fd2, fence; uint32_t flags, pad; } merge = {0};
    strcpy(merge.name, "prime-test");
    merge.fd2 = fence.fd;
    CHECK(ioctl(fence.fd, 0xc0303e03UL, &merge) == 0 && merge.fence >= 0);
    CHECK(fcntl(merge.fence, F_GETFD) & FD_CLOEXEC);
    struct { char name[32], driver[32]; int32_t status; uint32_t flags; uint64_t timestamp; } detail = {0};
    struct { char name[32]; int32_t status; uint32_t flags, count, pad; uint64_t details; } info = {0};
    info.count = 1; info.details = (uintptr_t)&detail;
    CHECK(ioctl(merge.fence, 0xc0383e04UL, &info) == 0);
    CHECK(info.status == 1 && info.count == 1 && detail.status == 1);
    CHECK(!strcmp(info.name, "prime-test") && !strcmp(detail.driver, "a20-sync"));
    info.flags = 1;
    errno = 0;
    CHECK(ioctl(merge.fence, 0xc0383e04UL, &info) == -1 && errno == EINVAL);
    merge.fd2 = drm;
    errno = 0;
    CHECK(ioctl(fence.fd, 0xc0303e03UL, &merge) == -1 && errno == EINVAL);
    struct { uint64_t deadline, pad; } deadline = {1, 0};
    CHECK(ioctl(fence.fd, 0x40103e05UL, &deadline) == 0);
    deadline.pad = 1;
    errno = 0;
    CHECK(ioctl(fence.fd, 0x40103e05UL, &deadline) == -1 && errno == EINVAL);
    CHECK(close(merge.fence) == 0);
    CHECK(close(fence.fd) == 0);
    int exports[80];
    for (unsigned i = 0; i < 80; i++) {
        struct prime many = {b.handle, O_CLOEXEC | O_RDWR, -1};
        CHECK(ioctl(drm, 0xc00c642dUL, &many) == 0);
        exports[i] = many.fd;
    }
    for (unsigned i = 0; i < 80; i++) CHECK(close(exports[i]) == 0);
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        close(sv[0]); close(p.fd); close(drm);
        _exit(consume(sv[1], b.size));
    }
    close(sv[1]);
    CHECK(transfer_fd(sv[0], p.fd) == 0);
    CHECK(close(p.fd) == 0);
    CHECK(ioctl(drm, 0xc00464b4UL, &b.handle) == 0);
    CHECK(write(sv[0], "G", 1) == 1);
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(original[1] == 0x87654321u && shared[1] == 0x87654321u);
    CHECK(munmap(original, b.size) == 0 && munmap(shared, b.size) == 0);
    close(sv[0]); close(drm);
    puts("DRM_PRIME: PASS shared-mapping+SCM_RIGHTS+export-lifetime+80-exports+sync-file");
    return 0;
}
