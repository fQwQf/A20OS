/*
 * gpu3d_test: exercise the A20 virtio-gpu 3D path end to end.
 *
 * Everything here goes through the DRM_IOCTL_VIRTGPU_* UAPI -- the interface
 * Mesa's virtio_gpu_dri.so actually speaks, and the part that decides whether
 * stock Mesa can attach at all.  So it gets exercised for real: capability
 * negotiation, capset retrieval, GEM allocation, a 3D resource with
 * host-visible backing, resource info round-trip, and finally a command
 * stream whose result is read back and compared against what was asked for.
 *
 * The read-back is the point.  An earlier revision submitted a 16-byte
 * zero-filled placeholder, reported whatever the host said as a note, and
 * still printed PASS -- proving only that bytes arrived, and making the gate
 * green whatever the host then did with them.  This one drives a real
 * VIRGL_CCMD_CLEAR against a real surface over that resource and checks the
 * pixels, so a stream the host dropped, misparsed, or rendered somewhere else
 * fails here rather than passing as "transport works".
 *
 * There used to be a cheaper private transport ABI underneath this, probed
 * first.  It is gone: every call it made is covered by the UAPI above, and
 * only the UAPI is the one Mesa speaks.  What it did provide that mattered is
 * kept -- the ability to tell "this device has no 3D" apart from "3D is
 * broken", now read from VIRTGPU_PARAM_3D_FEATURES instead.
 *
 * Exit codes are meaningful and must stay that way:
 *   0   PASS  -- virgl was present, the host accepted the stream, and the
 *               pixels match the colours this program asked for
 *   77  SKIP  -- nothing can be concluded: either no virgl was negotiated, or
 *               the host renderer could not bring up a GL context and so
 *               cannot produce pixels at all
 *               (autotools convention; distinct from PASS on purpose)
 *   1   FAIL  -- virgl is present and the host can render, but the stream was
 *               rejected or the pixels came back wrong
 *
 * The 77 case matters as much as the 0 one.  SKIP is deliberately not 0: an
 * earlier revision returned 0 on a 2D-only device, which made this test green
 * in every configuration and therefore carried no information at all.  For
 * the same reason a pixel mismatch on a host that never produced a capset is
 * SKIP and not FAIL -- that mismatch is the environment, and reporting it as
 * FAIL would be a red light carrying exactly as little information as the old
 * unconditional green one.
 *
 * Requires a QEMU virtio-gpu-gl device: build the instance with GPU_3D=1.
 * See docs/graphics/gpu-3d-roadmap.md.
 *
 * The command stream below is transcribed from virglrenderer 1.1.0 --
 * src/virgl_protocol.h, the vrend_decode.c decoders that consume it, and the
 * vendored p_defines.h for the clear bitmask.  That is the renderer QEMU loads
 * on the test host, so it is the code that will parse these bytes.  Field
 * offsets are relative to the command header because that is how the host
 * indexes them: vrend_decode reads the object type out of the header dword
 * itself and walks the stream by `len + 1` dwords.
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


/* A20-private: the Linux UAPI has no GEM_CREATE, so there is no upstream
 * number to match.  tools/check-drm-abi.sh checks the real DRM_* numbers here
 * against the installed UAPI headers. */
#define A20_GPU_IOCTL_GEM_CREATE 0x00004710UL
#define DRM_IOCTL_GEM_CLOSE   0x40086409UL
#define DRM_IOCTL_VIRTGPU_GETPARAM        0xc0106443UL
#define DRM_IOCTL_VIRTGPU_GET_CAPS        0xc0186449UL
#define DRM_IOCTL_VIRTGPU_MAP             0xc0106441UL
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
#define TEST_SIZE (TEST_W * TEST_H * TEST_BPP)

/* virgl_formats (virgl_hw.h).  QEMU indexes its format table with this, so
 * it is a virgl enum and not a DRM fourcc: the previous revision passed 0x8058
 * here, which is out of bounds for that table. */
#define VIRGL_FORMAT_B8G8R8A8_UNORM 1
/* VIRGL_BIND_RENDER_TARGET, not VIRGL_BIND_DEPTH_STENCIL: a clear needs a
 * colour attachment to land on. */
#define VIRGL_BIND_RENDER_TARGET 0x2

/* virgl_context_cmd (virgl_protocol.h); the enum is implicit from zero. */
#define CCMD_CREATE_OBJECT 1
#define CCMD_SET_FRAMEBUFFER_STATE 5
#define CCMD_CLEAR 7
/* enum virgl_object_type */
#define OBJECT_SURFACE 8
/* Command header: cmd in bits 0-7, object type 8-15, length in dwords 16-31. */
#define VIRGL_CMD0(cmd, obj, len) \
    ((uint32_t)(cmd) | ((uint32_t)(obj) << 8) | ((uint32_t)(len) << 16))
/* Payload sizes.  The host rejects a stream whose length does not match these
 * exactly, so they are contract, not preference. */
#define OBJ_SURFACE_SIZE 5
#define OBJ_CLEAR_SIZE 8
#define SET_FRAMEBUFFER_STATE_SIZE(n) ((n) + 2)
/* PIPE_CLEAR_COLOR0 */
#define PIPE_CLEAR_COLOR0 0x4


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

struct drm_virtgpu_map {
    uint64_t offset;
    uint32_t handle;
    uint32_t pad;
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

/* The guest's own handle for the surface object the stream creates.  Any
 * non-zero value works as long as it is used consistently within the stream;
 * the host rejects a create whose handle is 0. */
#define SURFACE_HANDLE 1

static uint32_t test_stream[24];

static uint32_t float_bits(float f)
{
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    return bits;
}

static int fail(const char *what)
{
    printf("GPU3D_TEST: FAIL %s errno=%d\n", what, errno);
    return EXIT_FAIL;
}

/* Build a stream that creates a surface over res_handle and clears it to
 * rgba.  Three commands, in the order vrend_decode requires:
 * CREATE_OBJECT(SURFACE) carries the resource and format, SET_FRAMEBUFFER_STATE
 * binds that surface as colour attachment 0, and CLEAR then acts on the bound
 * framebuffer.  Returns the byte length. */
static uint32_t build_clear_stream(uint32_t res_handle, float r, float g,
                                   float b, float a)
{
    uint32_t *s = test_stream;
    uint32_t n = 0;

    s[n++] = VIRGL_CMD0(CCMD_CREATE_OBJECT, OBJECT_SURFACE, OBJ_SURFACE_SIZE);
    s[n++] = SURFACE_HANDLE;                 /* VIRGL_OBJ_SURFACE_HANDLE */
    s[n++] = res_handle;                     /* VIRGL_OBJ_SURFACE_RES_HANDLE */
    s[n++] = VIRGL_FORMAT_B8G8R8A8_UNORM;    /* VIRGL_OBJ_SURFACE_FORMAT */
    s[n++] = 0;                              /* texture level */
    s[n++] = 0;                              /* layers: first 0, last 0 */

    s[n++] = VIRGL_CMD0(CCMD_SET_FRAMEBUFFER_STATE, 0,
                        SET_FRAMEBUFFER_STATE_SIZE(1));
    s[n++] = 1;                              /* nr_cbufs */
    s[n++] = 0;                              /* depth/stencil surface */
    s[n++] = SURFACE_HANDLE;                 /* colour buffer 0 */

    s[n++] = VIRGL_CMD0(CCMD_CLEAR, 0, OBJ_CLEAR_SIZE);
    s[n++] = PIPE_CLEAR_COLOR0;
    s[n++] = float_bits(r);
    s[n++] = float_bits(g);
    s[n++] = float_bits(b);
    s[n++] = float_bits(a);
    /* depth is a host-side double, little-endian low dword then high */
    s[n++] = 0;
    s[n++] = 0;
    s[n++] = 0;                              /* stencil */

    return n * (uint32_t)sizeof(uint32_t);
}

int main(void)
{
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return fail("open /dev/dri/card0");

    /* --- the VIRTGPU UAPI Mesa speaks --- */
    struct drm_virtgpu_getparam p;
    p.param = VIRTGPU_PARAM_3D_FEATURES;
    p.value = 0;
    if (ioctl(fd, DRM_IOCTL_VIRTGPU_GETPARAM, &p) < 0)
        return fail("VIRTGPU_GETPARAM 3D_FEATURES");
    /* SKIP rather than FAIL when the device negotiated no virgl.  The kernel
     * answers this from the feature bits it actually agreed with the host, so
     * 0 here means "there is no 3D path on this device" -- which is what a
     * GPU_3D=0 instance is -- and not "3D is broken".  Collapsing the two is
     * what made an earlier revision of this test green everywhere. */
    if (p.value != 1) {
        printf("GPU3D_TEST: SKIP 2D-only device (build with GPU_3D=1 to test 3D)\n");
        close(fd);
        return EXIT_SKIP;
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

    /* Capset retrieval doubles as the host's "can you actually render?"
     * probe.  virglrenderer builds its capsets out of the offscreen GL
     * context; when it cannot create one it answers GET_CAPSET with
     * ERR_INVALID_PARAMETER, and every context it hands out is born in an
     * error state -- a surface over a resource then fails for want of a GL id,
     * and the commands after it are dropped.  So a host that cannot return a
     * capset cannot produce pixels either, and a pixel mismatch there says
     * nothing about the encoding under test.  Remember the answer instead of
     * pretending the two situations are the same. */
    int capset_ok = 0;
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
        capset_ok = 1;
        printf("GPU3D_TEST: GET_CAPS returned a %u byte capset\n", caps.size);
    }

    /* A real GEM buffer, then a 3D resource on top of it.  RESOURCE_CREATE is
     * where the guest must publish its own pages to the host; if backing
     * attach were missing this is the call that would silently do nothing. */
    struct drm_gem_create g;
    g.width = TEST_W;
    g.height = TEST_H;
    g.format = VIRGL_FORMAT_B8G8R8A8_UNORM;
    g.bpp = TEST_BPP * 8;
    g.size = TEST_SIZE;
    g.handle = 0;
    if (ioctl(fd, A20_GPU_IOCTL_GEM_CREATE, &g) < 0)
        return fail("GEM_CREATE");
    if (g.handle == 0) {
        printf("GPU3D_TEST: FAIL GEM_CREATE returned handle 0\n");
        return EXIT_FAIL;
    }
    printf("GPU3D_TEST: GEM handle %u allocated (%u bytes)\n", g.handle, g.size);

    struct drm_virtgpu_resource_create rc;
    memset(&rc, 0, sizeof(rc));
    rc.target = 2; /* GL_TEXTURE_2D */
    rc.format = VIRGL_FORMAT_B8G8R8A8_UNORM;
    rc.bind = VIRGL_BIND_RENDER_TARGET;
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

    /* Map the resource so the host's writes are visible here.  These are the
     * same physical frames the host renders into, so this reads what the host
     * produced rather than a copy of what this program last wrote. */
    struct drm_virtgpu_map m;
    m.handle = g.handle;
    m.pad = 0;
    m.offset = 0;
    if (ioctl(fd, DRM_IOCTL_VIRTGPU_MAP, &m) < 0)
        return fail("VIRTGPU_MAP");
    uint32_t *pix = mmap(NULL, TEST_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
                         fd, (off_t)m.offset);
    if (pix == MAP_FAILED)
        return fail("mmap the virgl resource");

    /* A sentinel the host has to overwrite, so a colour that matches
     * afterwards cannot be something this program put there itself. */
    for (uint32_t i = 0; i < TEST_SIZE / 4; i++)
        pix[i] = 0xdeadbeef;

    /* Two colours, not one: if only the first pass were being checked, a
     * buffer that merely happened to hold the expected value would pass. */
    static const struct {
        float r, g, b, a;
        uint32_t expect;   /* BGRA8, little-endian: B,G,R,A by address */
    } passes[] = {
        { 1.0f, 0.0f, 0.0f, 1.0f, 0xff0000ffu },
        { 0.0f, 0.0f, 1.0f, 1.0f, 0xffff0000u },
    };

    for (unsigned i = 0; i < sizeof(passes) / sizeof(passes[0]); i++) {
        uint32_t len = build_clear_stream(rc.res_handle, passes[i].r, passes[i].g,
                                          passes[i].b, passes[i].a);

        struct drm_virtgpu_execbuffer eb;
        memset(&eb, 0, sizeof(eb));
        eb.flags = 0;
        eb.size = len;
        eb.command = (uint64_t)(uintptr_t)test_stream;
        eb.bo_handles = (uint64_t)(uintptr_t)&g.handle;
        eb.num_bo_handles = 1;
        eb.fence_fd = -1;
        if (ioctl(fd, DRM_IOCTL_VIRTGPU_EXECBUFFER, &eb) < 0) {
            /* A rejected stream is a failure, not a note.  It means the
             * encoding is wrong, which is what this test exists to catch, and
             * reporting PASS here is exactly what let the placeholder version
             * of this test pass in every configuration. */
            printf("GPU3D_TEST: FAIL host rejected a %u byte command stream "
                   "(errno=%d)\n", len, errno);
            return EXIT_FAIL;
        }
        printf("GPU3D_TEST: EXECBUFFER accepted a %u byte clear stream\n", len);

        uint32_t bad = 0;
        for (uint32_t p = 0; p < TEST_SIZE / 4; p++)
            if (pix[p] != passes[i].expect)
                bad++;
        if (bad) {
            /* A host that never produced a capset has no working renderer, so
             * a mismatch here is the environment, not the encoding.  Reporting
             * it as FAIL would be a red light carrying the same information as
             * the old green one; reporting it as PASS would be a lie. */
            if (!capset_ok) {
                printf("GPU3D_TEST: SKIP host cannot render (no capset), so the "
                       "pixel mismatch (%u/%u) is not attributable to the "
                       "command stream\n", bad, TEST_SIZE / 4);
                return EXIT_SKIP;
            }
            printf("GPU3D_TEST: FAIL pass %u: %u/%u pixels wrong, first pixel "
                   "is 0x%08x, expected 0x%08x\n",
                   i, bad, TEST_SIZE / 4, pix[0], passes[i].expect);
            return EXIT_FAIL;
        }
        printf("GPU3D_TEST: pixel readback ok (0x%08x across %u pixels)\n",
               passes[i].expect, TEST_SIZE / 4);
    }

    munmap(pix, TEST_SIZE);

    struct drm_gem_close cl;
    cl.handle = g.handle;
    cl.pad = 0;
    if (ioctl(fd, DRM_IOCTL_GEM_CLOSE, &cl) < 0)
        return fail("GEM_CLOSE");

    close(fd);
    printf("GPU3D_TEST: PASS (UAPI works and the host rendered the colours "
           "asked for)\n");
    return EXIT_PASS;
}
