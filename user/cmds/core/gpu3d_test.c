/*
 * gpu3d_test: exercise the A20 virtio-gpu 3D (virgl) transport path.
 *
 * Opens /dev/dri/card0 and drives the A20_GPU_IOCTL_* family directly:
 *   - VIRGL_CHECK:  verify the host offered VIRTIO_GPU_F_VIRGL
 *   - CTX_CREATE:   create a host-side virgl rendering context
 *   - RES_CREATE_3D: create a 3D resource in that context
 *   - RES_UNREF / CTX_DESTROY: teardown
 *
 * Exit codes are meaningful and must stay that way:
 *   0   PASS  -- a virgl device was present and every transport step was
 *                accepted by the host
 *   77  SKIP  -- the device is 2D-only, so there is no 3D path to test
 *                (autotools convention; distinct from PASS on purpose)
 *   1   FAIL
 *
 * SKIP is deliberately not 0.  An earlier revision of this test returned 0
 * on a 2D-only device, which made it green in every configuration and
 * therefore carried no information at all.  A verification step that cannot
 * fail is not a verification step.
 *
 * What this does NOT cover: no command stream is submitted, so it does not
 * prove that anything was rendered, and the created resource has no backing
 * attached (the host has no memory for it).  Until backing attach and a real
 * SUBMIT_3D land, treat this as a *transport reachability* check only.  See
 * docs/graphics/gpu-3d-roadmap.md sections 4.3, 4.4 and 5.
 *
 * Requires a QEMU virtio-gpu-gl device: build with GPU_3D=1.
 *
 * The ioctl codes and the request struct are duplicated here (rather than
 * including the kernel header) so this test builds standalone against the
 * musl userspace toolchain.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define A20_GPU_IOCTL_BASE         0x4700UL
#define A20_GPU_IOCTL_CTX_CREATE   (A20_GPU_IOCTL_BASE + 1)
#define A20_GPU_IOCTL_CTX_DESTROY  (A20_GPU_IOCTL_BASE + 2)
#define A20_GPU_IOCTL_RES_CREATE_3D (A20_GPU_IOCTL_BASE + 3)
#define A20_GPU_IOCTL_RES_UNREF    (A20_GPU_IOCTL_BASE + 4)
#define A20_GPU_IOCTL_SUBMIT_3D    (A20_GPU_IOCTL_BASE + 5)
#define A20_GPU_IOCTL_VIRGL_CHECK  (A20_GPU_IOCTL_BASE + 6)

#define EXIT_PASS 0
#define EXIT_FAIL 1
#define EXIT_SKIP 77

struct virtio_gpu_3d_req {
    uint32_t ctx_id;
    uint32_t resource_id;
    uint32_t target;
    uint32_t format;
    uint32_t bind;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t array_size;
    uint32_t last_level;
    uint32_t nr_samples;
    uint32_t flags;
    uint32_t context_init;
    uint64_t cmdbuf;
    uint64_t cmdlen;
    char     name[32];
};

static int fail(const char *what)
{
    printf("GPU3D_TEST: FAIL %s errno=%d\n", what, errno);
    return EXIT_FAIL;
}

int main(void)
{
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return fail("open /dev/dri/card0");

    struct virtio_gpu_3d_req r;
    memset(&r, 0, sizeof(r));

    if (ioctl(fd, A20_GPU_IOCTL_VIRGL_CHECK, &r) < 0) {
        if (errno == ENXIO) {
            printf("GPU3D_TEST: SKIP 2D-only device (build with GPU_3D=1 to test 3D)\n");
            close(fd);
            return EXIT_SKIP;
        }
        return fail("VIRGL_CHECK");
    }
    printf("GPU3D_TEST: virgl available\n");

    r.ctx_id = 1;
    r.context_init = 1;
    memcpy(r.name, "a20-gpu3d-test", 15);
    if (ioctl(fd, A20_GPU_IOCTL_CTX_CREATE, &r) < 0)
        return fail("CTX_CREATE");
    printf("GPU3D_TEST: context 1 created\n");

    memset(&r, 0, sizeof(r));
    r.ctx_id = 1;
    r.resource_id = 2;
    r.target = 2;          /* GL_TEXTURE_2D */
    r.format = 0x8058;     /* GL_RGBA8 */
    r.bind = 0x0001;       /* VIRGL_BIND_TEXTURE */
    r.width = 16;
    r.height = 16;
    r.depth = 1;
    r.array_size = 1;
    if (ioctl(fd, A20_GPU_IOCTL_RES_CREATE_3D, &r) < 0)
        return fail("RES_CREATE_3D");
    printf("GPU3D_TEST: 3D resource 2 created (16x16 RGBA8)\n");

    memset(&r, 0, sizeof(r));
    r.ctx_id = 1;
    r.resource_id = 2;
    if (ioctl(fd, A20_GPU_IOCTL_RES_UNREF, &r) < 0)
        return fail("RES_UNREF");
    printf("GPU3D_TEST: resource 2 released\n");

    memset(&r, 0, sizeof(r));
    r.ctx_id = 1;
    if (ioctl(fd, A20_GPU_IOCTL_CTX_DESTROY, &r) < 0)
        return fail("CTX_DESTROY");
    printf("GPU3D_TEST: context destroyed\n");

    close(fd);
    printf("GPU3D_TEST: PASS (transport only -- no command stream submitted, "
           "rendering unverified)\n");
    return EXIT_PASS;
}
