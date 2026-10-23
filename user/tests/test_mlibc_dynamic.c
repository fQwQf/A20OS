/*
 * Real dynamic Native program for the mlibc runtime gate.
 *
 * This binary is linked as PIE with PT_INTERP=/lib/ld.so and DT_NEEDED
 * libc.so.  The kernel loads the mlibc interpreter, and ld.so resolves the
 * NEEDED library (through $ORIGIN) and relocates both objects; no fake
 * loader is involved.
 *
 * Covered here:
 *   - libc calls through the shared object (snprintf/strlen/qsort);
 *   - per-thread TLS in the executable and in a dlopen()ed plugin;
 *   - pthread_create/pthread_join with the dynamic TCB and TLS;
 *   - dlopen/dlsym on a real shared object;
 *   - atexit and re-exec with the same image (the re-exec'd instance proves
 *     it ran by printing a marker and returning normally).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NTHREADS 4
#define ITERS 1000

extern char **environ;

static _Thread_local int tls_main;
static atomic_int shared_count;
static int failures;

static void fail(const char *what)
{
    printf("DYNAMIC: FAIL %s\n", what);
    failures++;
}

static void atexit_marker(void)
{
    printf("DYNAMIC: atexit\n");
    fflush(stdout);
}

static void *worker(void *arg)
{
    int id = (int)(long)arg;
    tls_main = id * 100;
    for (int i = 0; i < ITERS; i++) {
        tls_main++;
        atomic_fetch_add_explicit(&shared_count, 1, memory_order_relaxed);
    }
    return (void *)(long)tls_main;
}

static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a;
    int y = *(const int *)b;
    return (x > y) - (x < y);
}

static void test_libc_calls(void)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%s-%d", "dyn", 42);
    if (strcmp(buf, "dyn-42") != 0)
        fail("snprintf");
    if (strlen("abcdef") != 6)
        fail("strlen");

    int values[7] = { 7, 1, 5, 3, 2, 6, 4 };
    qsort(values, 7, sizeof(values[0]), cmp_int);
    for (int i = 0; i < 7; i++)
        if (values[i] != i + 1)
            fail("qsort");
}

static void test_threads(void)
{
    atomic_init(&shared_count, 0);
    pthread_t threads[NTHREADS];
    for (int i = 0; i < NTHREADS; i++) {
        if (pthread_create(&threads[i], NULL, worker, (void *)(long)i) != 0) {
            fail("pthread_create");
            return;
        }
    }
    for (int i = 0; i < NTHREADS; i++) {
        void *ret = NULL;
        if (pthread_join(threads[i], &ret) != 0) {
            fail("pthread_join");
            continue;
        }
        if ((long)ret != i * 100 + ITERS)
            fail("tls-per-thread");
    }
    if (atomic_load_explicit(&shared_count, memory_order_relaxed) !=
        NTHREADS * ITERS)
        fail("shared-counter");
}

struct plug {
    void *handle;
    int (*answer)(void);
    void (*tls_set)(int);
    int (*tls_get)(void);
};

struct plugin_job {
    struct plug *p;
    int id;
};

static void *plugin_worker(void *arg)
{
    struct plugin_job *job = arg;
    struct plug *p = job->p;
    /* A fresh thread starts with the module's initial TLS image (zero), not
     * with the parent thread's value. */
    if (p->tls_get() != 0)
        return (void *)-1L;
    p->tls_set(job->id * 10 + 3);
    for (int i = 0; i < 100; i++)
        p->tls_set(p->tls_get() + 1);
    return (void *)(long)p->tls_get();
}

static void test_plugin(void)
{
    struct plug p;
    memset(&p, 0, sizeof(p));
    p.handle = dlopen("/bin/lib/libmlibcplug.so", RTLD_NOW);
    if (!p.handle) {
        printf("DYNAMIC: dlerror=%s\n", dlerror());
        fail("dlopen");
        return;
    }
    p.answer = (int (*)(void))dlsym(p.handle, "plug_answer");
    p.tls_set = (void (*)(int))dlsym(p.handle, "plug_tls_set");
    p.tls_get = (int (*)(void))dlsym(p.handle, "plug_tls_get");
    if (!p.answer || !p.tls_set || !p.tls_get) {
        fail("dlsym");
        return;
    }
    if (p.answer() != 42)
        fail("plugin-call");
    if (p.tls_get() != 0)
        fail("plugin-tls-initial");

    pthread_t threads[NTHREADS];
    struct plugin_job jobs[NTHREADS];
    for (int i = 0; i < NTHREADS; i++) {
        jobs[i].p = &p;
        jobs[i].id = i;
        if (pthread_create(&threads[i], NULL, plugin_worker, &jobs[i]) != 0) {
            fail("plugin-pthread_create");
            return;
        }
    }
    for (int i = 0; i < NTHREADS; i++) {
        void *ret = NULL;
        if (pthread_join(threads[i], &ret) != 0) {
            fail("plugin-pthread_join");
            continue;
        }
        if ((long)ret != i * 10 + 3 + 100)
            fail("plugin-tls-per-thread");
    }
    /* The main thread's plugin TLS must be separate from the workers'. */
    p.tls_set(777);
    if (p.tls_get() != 777)
        fail("plugin-tls-main");
}

int main(int argc, char **argv)
{
    atexit(atexit_marker);

    if (argc > 1 && strcmp(argv[1], "reexec") == 0) {
        printf("DYNAMIC: reexec-ok argc=%d\n", argc);
        return 0;
    }

    printf("DYNAMIC: start\n");
    test_libc_calls();
    test_threads();
    test_plugin();

    if (failures) {
        printf("DYNAMIC: FAIL count=%d\n", failures);
        return 1;
    }
    printf("DYNAMIC: PASS\n");
    fflush(stdout);

    char *nargv[] = { "mlibc-dynamic-rv", "reexec", NULL };
    execve("/bin/mlibc-dynamic-rv", nargv, environ);
    printf("DYNAMIC: FAIL execve\n");
    return 2;
}
