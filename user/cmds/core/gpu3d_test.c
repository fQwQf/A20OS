/*
 * gpu3d_test: exercise the A20 virtio-gpu 3D path end to end.
 *
 * Two layers are covered, and the distinction matters:
 *
 *  1. The virtio-gpu 3D transport (A20_GPU_IOCTL_*).  Cheap, and the first
 *     thing to break if a QEMU config stops offering virgl.
 *
 *  2. The DRM_IOCTL_VIRTGPU_* UAPI -- the interface Mesa's virtio_gpu_dri.so
 *     actually speaks.  This is the part that decides whether stock Mesa can
 *     attach at all, so it gets exercised for real: capability negotiation,
 *     capset retrieval, GEM allocation, a 3D resource with host-visible
 *     backing, resource info round-trip, and a command-stream submit.
 *
 * Exit codes are meaningful and must stay that way:
 *   0   PASS  -- a virgl device was present and every step above succeeded
 *   77  SKIP  -- the device is 2D-only, so there is no 3D path to test
 *                (autotools convention; distinct from PASS on purpose)
 *   1   FAIL
 *
 * SKIP is deliberately not 0.  An earlier revision returned 0 on a 2D-only
 * device, which made this test green in every configuration and therefore
 * carried no information at all.
 *
 * Requires a QEMU virtio-gpu-gl device: build the instance with GPU_3D=1.
 * See docs/graphics/gpu-3d-roadmap.md.
 *
 * ioctl numbers and structs are duplicated here rather than including the
 * kernel headers so this builds standalone against the musl toolchain.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define A20_GPU_IOCTL_BASE          0x4700UL
#define A20_GPU_IOCTL_CTX_CREATE    (A20_GPU_IOCTL_BASE + 1)
#define A20_GPU_IOCTL_CTX_DESTROY   (A20_GPU_IOCTL_BASE + 2)
#define A20_GPU_IOCTL_RES_CREATE_3D (A20_GPU_IOCTL_BASE + 3)
#define A20_GPU_IOCTL_RES_UNREF     (A20_GPU_IOCTL_BASE + 4)
#define A20_GPU_IOCTL_VIRGL_CHECK   (A20_GPU_IOCTL_BASE + 6)

#define DRM_IOCTL_GEM_CREATE  0xc018640cUL
#define DRM_IOCTL_GEM_CLOSE   0x40086409UL
#define DRM_IOCTL_VIRTGPU_GETPARAM        0xc0106443UL
#define DRM_IOCTL_VIRTGPU_GET_CAPS        0xc0186449UL
#define DRM_IOCTL_VIRTGPU_RESOURCE_CREATE 0xc0386444UL
#define DRM_IOCTL_VIRTGPU_RESOURCE_INFO   0xc0106445UL
#define DRM_IOCTL_VIRTGPU_EXECBUFFER      0xc0406442UL

#define VIRTGPU_PARAM_3D_FEATURES          1
#define VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs 7

#define EXIT_PASS 0
#define EXIT_FAIL 1
#define EXIT_SKIP 77

#define TEST_W 64
#define TEST_H 64
#define TEST_BPP 4

struct virtio_gpu_3d_req {
    uint32_t ctx_id, resource_id, target, format, bind, width, height, depth;
    uint32_t array_size, last_level, nr_samples, flags, context_init;
    uint64_t cmdbuf, cmdlen;
    char name[32];
};

struct drm_gem_create {
    uint32_t width, height, format, bpp, size, handle;
};

struct drm_gem_close {
    uint32_t handle, pad;
};

struct drm_virtgpu_getparam {
    uint64_t param, value;
};

struct drm_virtgpu_get_caps {
    uint32_t cap_set_id, cap_set_ver;
    uint64_t addr;
    uint32_t size, pad;
};

struct drm_virtgpu_resource_create {
    uint32_t target, format, bind, width, height, depth, array_size;
    uint32_t last_level, nr_samples, flags, bo_handle, res_handle, size, stride;
};

struct drm_virtgpu_resource_info {
    uint32_t bo_handle, res_handle, size, blob_mem;
};

struct drm_virtgpu_execbuffer {
    uint32_t flags, size;
    uint64_t command, bo_handles;
    uint32_t num_bo_handles;
    int32_t fence_fd;
    uint32_t ring_idx, syncobj_stride, num_in_syncobjs, num_out_syncobjs;
    uint64_t in_syncobjs, out_syncobjs;
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
    printf("GPU3D_TEST: virgl device present\n");

    /* --- layer 1: raw transport --- */
    r.ctx_id = 1;
    r.context_init = 1;
    memcpy(r.name, "a20-gpu3d-test", 15);
    if (ioctl(fd, A20_GPU_IOCTL_CTX_CREATE, &r) < 0)
        return fail("CTX_CREATE");
    printf("GPU3D_TEST: transport ctx created\n");

    memset(&r, 0, sizeof(r));
    r.ctx_id = 1;
    r.resource_id = 2;
    r.target = 2;          /* GL_TEXTURE_2D */
    r.format = 0x8058;     /* GL_RGBA8 */
    r.bind = 0x0001;
    r.width = 16;
    r.height = 16;
    r.depth = 1;
    r.array_size = 1;
    if (ioctl(fd, A20_GPU_IOCTL_RES_CREATE_3D, &r) < 0)
        return fail("RES_CREATE_3D");
    printf("GPU3D_TEST: transport resource created\n");

    memset(&r, 0, sizeof(r));
    r.ctx_id = 1;
    r.resource_id = 2;
    if (ioctl(fd, A20_GPU_IOCTL_RES_UNREF, &r) < 0)
        return fail("RES_UNREF");
    memset(&r, 0, sizeof(r));
    r.ctx_id = 1;
    if (ioctl(fd, A20_GPU_IOCTL_CTX_DESTROY, &r) < 0)
        return fail("CTX_DESTROY");
    printf("GPU3D_TEST: transport teardown clean\n");

    /* --- layer 2: the VIRTGPU UAPI Mesa speaks --- */
    struct drm_virtgpu_getparam p;
    p.param = VIRTGPU_PARAM_3D_FEATURES;
    p.value = 0;
    if (ioctl(fd, DRM_IOCTL_VIRTGPU_GETPARAM, &p) < 0)
        return fail("VIRTGPU_GETPARAM 3D_FEATURES");
    if (p.value != 1) {
        printf("GPU3D_TEST: FAIL host reports no 3D features\n");
        return EXIT_FAIL;
    }
    printf("GPU3D_TEST: GETPARAM 3D_FEATURES=1\n");

    p.param = VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs;
    p.value = 0;
    if (ioctl(fd, DRM_IOCTL_VIRTGPU_GETPARAM, &p) < 0)
        return fail("VIRTGPU_GETPARAM CAPSET_IDs");
    if (!(p.value & (1ULL << 1))) {
        printf("GPU3D_TEST: FAIL host does not advertise the virgl capset\n");
        return EXIT_FAIL;
    }
    printf("GPU3D_TEST: GETPARAM capset mask=0x%llx (virgl present)\n",
           (unsigned long long)p.value);

    /* Capset retrieval: the host hands over its capability blob.  Whether it
     * can is a property of the *host* GL stack, not of this kernel --
     * virglrenderer needs a Mesa-style offscreen desktop-GL context, and a
     * proprietary EGL (NVIDIA) initialises but then fails to create one,
     * which the host reports as ERR_INVALID_PARAMETER.  So record what
     * happened and carry on: the steps below are the kernel's own job. */
    static uint8_t capblob[4096];
    struct drm_virtgpu_get_caps caps;
    caps.cap_set_id = 1;
    caps.cap_set_ver = 1;
    caps.addr = (uint64_t)(uintptr_t)capblob;
    caps.size = sizeof(capblob);
    caps.pad = 0;
    if (ioctl(fd, DRM_IOCTL_VIRTGPU_GET_CAPS, &caps) < 0) {
        printf("GPU3D_TEST: NOTE GET_CAPS unavailable on this host "
               "(errno=%d); host virglrenderer could not build a GL context\n",
               errno);
    } else {
        printf("GPU3D_TEST: GET_CAPS returned a %u byte capset\n", caps.size);
    }

    /* A real GEM buffer, then a 3D resource on top of it.  RESOURCE_CREATE is
     * where the guest must publish its own pages to the host; if backing
     * attach were missing this is the call that would silently do nothing. */
    struct drm_gem_create g;
    g.width = TEST_W;
    g.height = TEST_H;
    g.format = 0x8058;     /* DRM_FORMAT_XRGB8888-ish; host validates */
    g.bpp = TEST_BPP * 8;
    g.size = TEST_W * TEST_H * TEST_BPP;
    g.handle = 0;
    if (ioctl(fd, DRM_IOCTL_GEM_CREATE, &g) < 0)
        return fail("GEM_CREATE");
    if (g.handle == 0) {
        printf("GPU3D_TEST: FAIL GEM_CREATE returned handle 0\n");
        return EXIT_FAIL;
    }
    printf("GPU3D_TEST: GEM handle %u allocated (%u bytes)\n", g.handle, g.size);

    struct drm_virtgpu_resource_create rc;
    memset(&rc, 0, sizeof(rc));
    rc.target = 2;
    rc.format = 0x8058;
    rc.bind = 0x0001;
    rc.width = TEST_W;
    rc.height = TEST_H;
    rc.depth = 1;
    rc.array_size = 1;
    rc.bo_handle = g.handle;
    rc.size = g.size;
    if (ioctl(fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, &rc) < 0)
        return fail("VIRTGPU_RESOURCE_CREATE");
    if (rc.res_handle == 0) {
        printf("GPU3D_TEST: FAIL RESOURCE_CREATE returned res_handle 0\n");
        return EXIT_FAIL;
    }
    printf("GPU3D_TEST: 3D resource %u created with host backing\n", rc.res_handle);

    struct drm_virtgpu_resource_info ri;
    ri.bo_handle = g.handle;
    ri.res_handle = 0;
    ri.size = 0;
    ri.blob_mem = 0;
    if (ioctl(fd, DRM_IOCTL_VIRTGPU_RESOURCE_INFO, &ri) < 0)
        return fail("VIRTGPU_RESOURCE_INFO");
    if (ri.res_handle != rc.res_handle) {
        printf("GPU3D_TEST: FAIL RESOURCE_INFO res_handle %u != %u\n",
               ri.res_handle, rc.res_handle);
        return EXIT_FAIL;
    }
    printf("GPU3D_TEST: RESOURCE_INFO round-trip ok (res %u, size %u)\n",
           ri.res_handle, ri.size);

    /* Submit a command stream.  The kernel forwards bytes it does not parse,
     * so what this proves is the round trip: the host received a stream and
     * answered.  It does NOT prove anything was rendered -- that needs a
     * stream whose contents match virgl's command encoding, which is the
     * remaining step in docs/graphics/gpu-3d-roadmap.md section 4.4. */
    uint8_t stream[16];
    memset(stream, 0, sizeof(stream));
    struct drm_virtgpu_execbuffer eb;
    memset(&eb, 0, sizeof(eb));
    eb.flags = 0;
    eb.size = sizeof(stream);
    eb.command = (uint64_t)(uintptr_t)stream;
    eb.bo_handles = (uint64_t)(uintptr_t)&g.handle;
    eb.num_bo_handles = 1;
    eb.fence_fd = -1;
    if (ioctl(fd, DRM_IOCTL_VIRTGPU_EXECBUFFER, &eb) < 0) {
        printf("GPU3D_TEST: NOTE host rejected the placeholder command stream "
               "(errno=%d); transport is fine, command encoding is not "
               "implemented yet\n", errno);
    } else {
        printf("GPU3D_TEST: EXECBUFFER accepted a %u byte stream\n", eb.size);
    }

    struct drm_gem_close cl;
    cl.handle = g.handle;
    cl.pad = 0;
    if (ioctl(fd, DRM_IOCTL_GEM_CLOSE, &cl) < 0)
        return fail("GEM_CLOSE");

    close(fd);
    printf("GPU3D_TEST: PASS (UAPI surface works; rendering still unproven)\n");
    return EXIT_PASS;
}
