/*
 * Host-side reproduction of what QEMU+virglrenderer do with A20OS's 3D path.
 *
 * Why: the guest gate can only say "the host accepted a 76 byte clear stream
 * and the pixels did not change".  That leaves the two interesting questions
 * open -- is the stream wrong, or is the plumbing wrong -- and answering either
 * through QEMU costs a multi-minute boot per attempt.  This does the same calls
 * in-process, so a failing case can be iterated on in seconds.
 *
 * It mirrors QEMU's sequence exactly:
 *   virgl_renderer_init() -> context_create -> resource_create -> attach_iov
 *   -> submit_cmd
 * using VIRGL_RENDERER_USE_EGL so virglrenderer brings up its own EGL winsys
 * (QEMU instead supplies its console GL context through callbacks; the decode
 * path after that is identical).
 *
 * Build/run: see the invocation in tools/virgl-probe/README.md.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <stdbool.h>
#include "virgl_hw.h"
#include "virglrenderer.h"

#define VIRGL_OBJ_SURFACE_HANDLE       1
#define VIRGL_OBJ_SURFACE_RES_HANDLE   2
#define VIRGL_OBJ_SURFACE_FORMAT       3
#define VIRGL_OBJ_SURFACE_LAST_LEVEL   4
#define VIRGL_OBJ_SURFACE_NUM_LAYERS   5
#define VIRGL_OBJ_SURFACE_SIZE         5

#define VIRGL_OBJ_FRAMEBUFFER_NR_CBUFS      1
#define VIRGL_OBJ_FRAMEBUFFER_DEPTH_STENCIL 2
#define VIRGL_OBJ_FRAMEBUFFER_ATTACHMENT_0 3

#define VIRGL_OBJ_CLEAR_BUFFERS  1
#define VIRGL_OBJ_CLEAR_COLOR_0  2
#define VIRGL_OBJ_CLEAR_DEPTH_0  6
#define VIRGL_OBJ_CLEAR_STENCIL  8
#define VIRGL_OBJ_CLEAR_SIZE     8

#define VIRGL_CCMD_CREATE_OBJECT         1
#define VIRGL_CCMD_SET_FRAMEBUFFER_STATE 5
#define VIRGL_CCMD_CLEAR                  7
#define VIRGL_OBJECT_SURFACE             8

#define VIRGL_FORMAT_B8G8R8A8_UNORM 2
#define PIPE_CLEAR_COLOR0            0x4

#define VIRGL_TARGET_TEXTURE_2D_ARRAY 0   /* VIRGL_TEXTURE_TARGET_2D */

#define CMD0(cmd, obj, len) ((uint32_t)(cmd) | ((uint32_t)(obj) << 8) | \
                            ((uint32_t)(len) << 16))

#define SENTINEL 0xdeadbeefu

static uint32_t g_stream[32];
static uint32_t g_page[4096];

static uint32_t float_bits(float f)
{
    union { float f; uint32_t u; } c;
    c.f = f;
    return c.u;
}

/* Byte-for-byte the stream user/cmds/core/gpu3d_test.c builds. */
static uint32_t build_clear_stream(uint32_t res_handle)
{
    uint32_t *s = g_stream, n = 0;

    s[n++] = CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE,
                  VIRGL_OBJ_SURFACE_SIZE);
    s[n++] = 1;                        /* VIRGL_OBJ_SURFACE_HANDLE */
    s[n++] = res_handle;               /* VIRGL_OBJ_SURFACE_RES_HANDLE */
    s[n++] = VIRGL_FORMAT_B8G8R8A8_UNORM;
    s[n++] = 0;                        /* texture level */
    s[n++] = 0;                        /* layers */

    s[n++] = CMD0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3); /* 2 + nr_cbufs */
    s[n++] = 1;                        /* nr_cbufs */
    s[n++] = 0;                        /* depth/stencil surface */
    s[n++] = 1;                        /* colour buffer 0 = surface handle */

    s[n++] = CMD0(VIRGL_CCMD_CLEAR, 0, VIRGL_OBJ_CLEAR_SIZE);
    s[n++] = PIPE_CLEAR_COLOR0;        /* VIRGL_OBJ_CLEAR_BUFFERS */
    s[n++] = float_bits(1.0f);         /* r */
    s[n++] = float_bits(0.0f);         /* g */
    s[n++] = float_bits(0.0f);         /* b */
    s[n++] = float_bits(1.0f);         /* a */
    s[n++] = 0;                        /* depth lo */
    s[n++] = 0;                        /* depth hi */
    s[n++] = 0;                        /* stencil */

    return n;
}


/* --- the GL context virglrenderer will render through ------------------------
 * QEMU supplies these through its console GL machinery; standalone consumers
 * have to bring their own.  A surfaceless EGL context is the smallest thing
 * that satisfies vrend, and it is the same context the host recipe hands QEMU.
 */
static EGLDisplay g_dpy;
static EGLConfig  g_cfg;
static EGLContext g_ctx;

static void *probe_get_egl_display(void *cookie)
{
    (void)cookie;
    return g_dpy;
}

static virgl_renderer_gl_context probe_create_gl_context(void *cookie,
                                                       int scanout_idx,
                                                       struct virgl_renderer_gl_ctx_param *param)
{
    (void)cookie; (void)scanout_idx;
    EGLint ctx_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, param && param->major_ver ? param->major_ver : 3,
        EGL_CONTEXT_MINOR_VERSION, param && param->minor_ver ? param->minor_ver : 3,
        EGL_NONE
    };
    if (param && param->compat_ctx)
        g_ctx = eglCreateContext(g_dpy, g_cfg, EGL_NO_CONTEXT, NULL);
    else
        g_ctx = eglCreateContext(g_dpy, g_cfg, EGL_NO_CONTEXT, ctx_attribs);
    return (virgl_renderer_gl_context)(intptr_t)1;
}

static void probe_destroy_gl_context(void *cookie, virgl_renderer_gl_context ctx)
{
    (void)cookie; (void)ctx;
    if (g_ctx != EGL_NO_CONTEXT) { eglDestroyContext(g_dpy, g_ctx); g_ctx = EGL_NO_CONTEXT; }
}

static int probe_make_current(void *cookie, int scanout_idx,
                              virgl_renderer_gl_context ctx)
{
    (void)cookie; (void)scanout_idx; (void)ctx;
    return eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g_ctx) ? 0 : -EINVAL;
}

static struct virgl_renderer_callbacks g_cbs = {
    .version = 4,
    .create_gl_context  = probe_create_gl_context,
    .destroy_gl_context = probe_destroy_gl_context,
    .make_current       = probe_make_current,
    .get_egl_display    = probe_get_egl_display,
};

int main(void)
{
    memset(g_page, 0xff, sizeof(g_page));       /* not the sentinel: real pattern */
    for (int i = 0; i < 4096; i++)
        g_page[i] = SENTINEL;

    /* Let virglrenderer bring up its own EGL winsys, the way a standalone
     * consumer does.  Forcing Mesa here mirrors tools/with-virgl-display.sh. */
    setenv("__EGL_VENDOR_LIBRARY_FILENAMES",
           "/usr/share/glvnd/egl_vendor.d/50_mesa.json", 1);
    setenv("LIBGL_ALWAYS_INDIRECT", "0", 1);

    g_dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA,
                                 EGL_DEFAULT_DISPLAY, NULL);
    if (g_dpy == EGL_NO_DISPLAY) { printf("RESULT: FAIL no surfaceless EGL\n"); return 1; }
    EGLint maj, min;
    if (!eglInitialize(g_dpy, &maj, &min)) { printf("RESULT: FAIL eglInitialize\n"); return 1; }
    eglBindAPI(EGL_OPENGL_API);
    static const EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLint ncfg = 0;
    if (!eglChooseConfig(g_dpy, cfg_attribs, &g_cfg, 1, &ncfg) || ncfg < 1) {
        printf("RESULT: FAIL eglChooseConfig\n"); return 1;
    }
    printf("probe: EGL %d.%d config ok\n", maj, min);

    printf("probe: renderer init\n");
    if (virgl_renderer_init((void *)&g_cbs, 0, &g_cbs) != 0) {
        printf("RESULT: FAIL renderer_init\n");
        return 1;
    }

    uint32_t max_ver = 0, max_size = 0;
    virgl_renderer_get_cap_set(1, &max_ver, &max_size);
    printf("probe: capset 1 max_ver=%u max_size=%u\n", max_ver, max_size);
    if (!max_size) {
        printf("RESULT: FAIL capset1_unavailable\n");
        return 1;
    }

    const uint32_t ctx_id = 1, res_id = 2;
    int rc = virgl_renderer_context_create(ctx_id, 0, "probe");
    printf("probe: context_create(%u) -> %d\n", ctx_id, rc);
    if (rc != 0) {
        printf("RESULT: FAIL context_create\n");
        return 1;
    }

    struct virgl_renderer_resource_create_args a;
    memset(&a, 0, sizeof(a));
    a.handle = res_id;
    a.target = 2; /* PIPE_TEXTURE_2D (PIPE_BUFFER is 0) */
    a.format = VIRGL_FORMAT_B8G8R8A8_UNORM;
    a.bind = VIRGL_BIND_SAMPLER_VIEW | VIRGL_BIND_RENDER_TARGET;
    a.width = 64;
    a.height = 64;
    a.depth = 1;
    a.array_size = 1;
    a.last_level = 0;
    a.nr_samples = 0;
    a.flags = 0;

    printf("probe: target=%u format=%u bind=0x%x w=%u h=%u d=%u arr=%u lvl=%u\n",
           a.target, a.format, a.bind, a.width, a.height, a.depth, a.array_size, a.last_level);
    rc = virgl_renderer_resource_create(&a, NULL, 0);
    printf("probe: resource_create(%u) -> %d\n", res_id, rc);
    if (rc != 0) {
        printf("RESULT: FAIL resource_create\n");
        return 1;
    }

    struct iovec iov = { .iov_base = g_page, .iov_len = sizeof(g_page) };
    rc = virgl_renderer_resource_attach_iov(res_id, &iov, 1);
    printf("probe: attach_iov(%u, 1 iov) -> %d\n", res_id, rc);
    if (rc != 0) {
        printf("RESULT: FAIL attach_iov\n");
        return 1;
    }

    uint32_t ndw = build_clear_stream(res_id);
    printf("probe: submit_cmd(%u dwords, %u bytes)\n", ndw, ndw * 4);
    rc = virgl_renderer_submit_cmd(g_stream, (int)ctx_id, (int)ndw);
    printf("probe: submit_cmd -> %d\n", rc);

    virgl_renderer_submit_cmd(NULL, 0, 0);

    printf("probe: page0[0] = 0x%08x (want 0xff0000ff-ish, i.e. r=1.0 b=0.0 a=1.0)\n",
           g_page[0]);
    if (g_page[0] == SENTINEL) {
        printf("RESULT: FAIL untouched -- host did not render into the backing\n");
        return 1;
    }
    printf("RESULT: PASS host rendered into the attached backing\n");
    return 0;
}