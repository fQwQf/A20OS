/*
 * test_pthread_mutex.c — Tests for A20 pthread + mutex bridge.
 *
 * Tests pthread_create/join/exit + mutex lock/unlock/trylock.
 * On A20: uses real thread_create/thread_exit syscalls.
 * On host: uses native pthreads for comparison.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef TEST_HOST

#include <pthread.h>
#define a20_pthread_t       pthread_t
#define a20_pthread_mutex_t pthread_mutex_t
#define a20_pthread_attr_t  pthread_attr_t
#define A20_PTHREAD_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER

#define a20_pthread_create   pthread_create
#define a20_pthread_join     pthread_join
#define a20_pthread_exit     pthread_exit
#define a20_pthread_self     pthread_self
#define a20_mutex_lock       pthread_mutex_lock
#define a20_mutex_unlock     pthread_mutex_unlock
#define a20_mutex_trylock    pthread_mutex_trylock
#define a20_mutex_init       pthread_mutex_init
#define a20_mutex_destroy    pthread_mutex_destroy

#else

/* A20 native: use our bridge */
#define pthread_t       a20_pthread_t
#define pthread_mutex_t a20_pthread_mutex_t
/* Pull in from a20_pthread.c / a20_mutex.c */

#endif

/* ---- Test infrastructure ---- */
static volatile int __test_pass = 0;
static volatile int __test_fail = 0;

#define ASSERT(expr, msg) do { \
    if (!(expr)) { \
        printf("  FAIL: %s (line %d)\n", msg, __LINE__); \
        __test_fail++; \
    } else { \
        __test_pass++; \
    } \
} while(0)

/* ---- Shared state for tests ---- */
static volatile int shared_counter;
static a20_pthread_mutex_t shared_mutex;

/* ---- test: basic create/join ---- */
static void *thread_basic(void *arg)
{
    int *val = (int *)arg;
    *val = 42;
    return (void *)(long)(*val + 1);
}

static void test_create_join(void)
{
    printf("[test] pthread create + join...\n");

    a20_pthread_t t;
    int arg = 0;
    void *ret = NULL;

    int rc = a20_pthread_create(&t, NULL, thread_basic, &arg);
    ASSERT(rc == 0, "pthread_create returns 0");

    rc = a20_pthread_join(t, &ret);
    ASSERT(rc == 0, "pthread_join returns 0");
    ASSERT(arg == 42, "thread set arg to 42");
    ASSERT((long)ret == 43, "thread returned 43");
}

/* ---- test: mutex exclusion ---- */
static void *thread_mutex_incr(void *arg)
{
    long n = (long)arg;
    for (long i = 0; i < n; i++) {
        a20_mutex_lock(&shared_mutex);
        shared_counter++;
        a20_mutex_unlock(&shared_mutex);
    }
    return NULL;
}

static void test_mutex_exclusion(void)
{
    printf("[test] mutex exclusion (1000 increments x 4 threads)...\n");

    shared_counter = 0;
    a20_mutex_init(&shared_mutex, NULL);

    a20_pthread_t threads[4];
    long incr_per_thread = 1000;

    for (int i = 0; i < 4; i++) {
        int rc = a20_pthread_create(&threads[i], NULL,
                                     thread_mutex_incr,
                                     (void *)incr_per_thread);
        ASSERT(rc == 0, "created thread for mutex test");
    }

    for (int i = 0; i < 4; i++) {
        a20_pthread_join(threads[i], NULL);
    }

    ASSERT(shared_counter == 4000, "all increments accounted for");
}

/* ---- test: trylock ---- */
static void test_mutex_trylock(void)
{
    printf("[test] mutex trylock...\n");

    a20_pthread_mutex_t m;
    a20_mutex_init(&m, NULL);

    int rc = a20_mutex_trylock(&m);
    ASSERT(rc == 0, "trylock on unlocked mutex succeeds");

    rc = a20_mutex_trylock(&m);
    ASSERT(rc != 0, "trylock on locked mutex fails");

    a20_mutex_unlock(&m);

    rc = a20_mutex_trylock(&m);
    ASSERT(rc == 0, "trylock after unlock succeeds");

    a20_mutex_unlock(&m);
    a20_mutex_destroy(&m);
}

/* ---- test: multiple joiners ---- */
static void *thread_noop(void *arg)
{
    (void)arg;
    return (void *)0xDEAD;
}

static void test_multiple_threads(void)
{
    printf("[test] 10 threads create + join...\n");

    a20_pthread_t threads[10];
    for (int i = 0; i < 10; i++) {
        int rc = a20_pthread_create(&threads[i], NULL, thread_noop, NULL);
        ASSERT(rc == 0, "created thread");
    }

    for (int i = 0; i < 10; i++) {
        void *ret = NULL;
        int rc = a20_pthread_join(threads[i], &ret);
        ASSERT(rc == 0, "joined thread");
    }
}

int main(void)
{
    printf("=== A20 pthread + mutex tests ===\n\n");

    test_create_join();
    test_mutex_exclusion();
    test_mutex_trylock();
    test_multiple_threads();

    printf("\n=== Results: %d passed, %d failed ===\n",
           __test_pass, __test_fail);
    return __test_fail ? 1 : 0;
}
