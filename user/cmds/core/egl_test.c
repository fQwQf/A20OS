/* Minimal EGL + GLES2 smoke test: initialize EGL on the surfaceless
 * platform and do a trivial glClear to prove Mesa EGL/GLES works. */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY) {
        printf("EGL_TEST: no display\n");
        return 1;
    }
    EGLint major, minor;
    if (!eglInitialize(dpy, &major, &minor)) {
        printf("EGL_TEST: init failed\n");
        return 1;
    }
    printf("EGL_TEST: EGL %d.%d\n", (int)major, (int)minor);

    static const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };
    EGLConfig cfg;
    EGLint n;
    if (!eglChooseConfig(dpy, config_attribs, &cfg, 1, &n) || n < 1) {
        printf("EGL_TEST: no config\n");
        return 1;
    }

    static const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
    if (ctx == EGL_NO_CONTEXT) {
        printf("EGL_TEST: no context\n");
        return 1;
    }
    EGLint pbuf_attribs[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pbuf_attribs);
    if (surf == EGL_NO_SURFACE) {
        printf("EGL_TEST: no surface\n");
        return 1;
    }
    if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
        printf("EGL_TEST: makecurrent failed\n");
        return 1;
    }
    glClearColor(0.2f, 0.4f, 0.6f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    GLuint fb = 0;
    glGenFramebuffers(1, &fb);
    printf("EGL_TEST: context current, glClear ok (fb=%u)\n", fb);
    eglTerminate(dpy);
    printf("EGL_TEST: PASS\n");
    return 0;
}
