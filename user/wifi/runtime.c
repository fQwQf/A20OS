/* SPDX-License-Identifier: BSD-3-Clause
 * POSIX services for the separately linked rtw89 userspace driver. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

void *IOMalloc(size_t size) { return malloc(size); }
void *IOMallocZero(size_t size) { return calloc(1, size); }
void IOFree(void *memory, size_t size) { (void)size; free(memory); }
int a20_wifi_kmsg_fd = -1;

void IOLog(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    char message[1024];
    int length = vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (length <= 0)
        return;
    size_t count = (size_t)length < sizeof(message) ? (size_t)length : sizeof(message) - 1;
    (void)fwrite(message, 1, count, stderr);
    if (a20_wifi_kmsg_fd >= 0)
        (void)write(a20_wifi_kmsg_fd, message, count);
}

void IODelay(unsigned microseconds)
{
    struct timespec delay = {microseconds / 1000000U,
                             (microseconds % 1000000U) * 1000U};
    while (nanosleep(&delay, &delay) && errno == EINTR) {}
}

void IOSleep(unsigned milliseconds)
{
    struct timespec delay = {milliseconds / 1000U,
                             (milliseconds % 1000U) * 1000000U};
    while (nanosleep(&delay, &delay) && errno == EINTR) {}
}

int scnprintf(char *buffer, size_t size, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int length = vsnprintf(buffer, size, format, args);
    va_end(args);
    if (!size || length < 0)
        return 0;
    return (size_t)length < size ? length : (int)size - 1;
}

void read_random(void *buffer, unsigned size)
{
    unsigned char *bytes = buffer;
    while (size) {
        ssize_t count = getrandom(bytes, size, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            perror("rtw89: getrandom");
            abort();
        }
        bytes += count;
        size -= (unsigned)count;
    }
}

void arc4random_buf(void *buffer, size_t size)
{
    while (size) {
        unsigned chunk = size > UINT32_MAX ? UINT32_MAX : (unsigned)size;
        read_random(buffer, chunk);
        buffer = (unsigned char *)buffer + chunk;
        size -= chunk;
    }
}

int PE_parse_boot_argn(const char *name, void *out, int size)
{
    const char *value = getenv(name);
    if (!value || size != sizeof(unsigned))
        return 0;
    unsigned number = (unsigned)strtoul(value, NULL, 0);
    memcpy(out, &number, sizeof(number));
    return 1;
}

uint64_t mach_absolute_time(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now))
        abort();
    return (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;
}

void absolutetime_to_nanoseconds(uint64_t value, uint64_t *out) { *out = value; }
void clock_interval_to_deadline(uint32_t interval, uint32_t scale, uint64_t *out)
{
    *out = mach_absolute_time() + (uint64_t)interval * scale;
}

struct runtime_lock {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    _Atomic uintptr_t owner;
    unsigned depth;
};

static void *lock_alloc(bool recursive)
{
    struct runtime_lock *lock = calloc(1, sizeof(*lock));
    if (!lock)
        return NULL;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, recursive ? PTHREAD_MUTEX_RECURSIVE :
                                               PTHREAD_MUTEX_ERRORCHECK);
    int error = pthread_mutex_init(&lock->mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    if (error) {
        free(lock);
        return NULL;
    }
    if (pthread_cond_init(&lock->changed, NULL)) {
        pthread_mutex_destroy(&lock->mutex);
        free(lock);
        return NULL;
    }
    return lock;
}

static void lock_take(struct runtime_lock *lock)
{
    if (pthread_mutex_lock(&lock->mutex))
        abort();
    lock->depth++;
    atomic_store(&lock->owner, (uintptr_t)pthread_self());
}

static void lock_release(struct runtime_lock *lock)
{
    if (!--lock->depth)
        atomic_store(&lock->owner, 0);
    if (pthread_mutex_unlock(&lock->mutex))
        abort();
}

static int lock_try(struct runtime_lock *lock)
{
    int error = pthread_mutex_trylock(&lock->mutex);
    if (!error) {
        lock->depth++;
        atomic_store(&lock->owner, (uintptr_t)pthread_self());
    }
    return !error;
}

static void lock_free(struct runtime_lock *lock)
{
    pthread_cond_destroy(&lock->changed);
    pthread_mutex_destroy(&lock->mutex);
    free(lock);
}

void *IOLockAlloc(void) { return lock_alloc(false); }
void IOLockFree(void *lock) { lock_free(lock); }
void IOLockLock(void *lock) { lock_take(lock); }
void IOLockUnlock(void *lock) { lock_release(lock); }
int IOLockTryLock(void *lock) { return lock_try(lock); }

int IOLockSleep(void *opaque, void *event, unsigned interruptible)
{
    (void)event;
    (void)interruptible;
    struct runtime_lock *lock = opaque;
    lock->depth = 0;
    atomic_store(&lock->owner, 0);
    int error = pthread_cond_wait(&lock->changed, &lock->mutex);
    lock->depth = 1;
    atomic_store(&lock->owner, (uintptr_t)pthread_self());
    return error;
}

void IOLockWakeup(void *opaque, void *event, int one)
{
    (void)event;
    struct runtime_lock *lock = opaque;
    if (one)
        pthread_cond_signal(&lock->changed);
    else
        pthread_cond_broadcast(&lock->changed);
}

void *IOSimpleLockAlloc(void) { return lock_alloc(false); }
void IOSimpleLockFree(void *lock) { lock_free(lock); }
void IOSimpleLockLock(void *lock) { lock_take(lock); }
void IOSimpleLockUnlock(void *lock) { lock_release(lock); }
unsigned long IOSimpleLockLockDisableInterrupt(void *lock)
{
    lock_take(lock);
    return 0;
}
void IOSimpleLockUnlockEnableInterrupt(void *lock, unsigned long flags)
{
    (void)flags;
    lock_release(lock);
}
void *IORecursiveLockAlloc(void) { return lock_alloc(true); }
void IORecursiveLockFree(void *lock) { lock_free(lock); }
void IORecursiveLockLock(void *lock) { lock_take(lock); }
void IORecursiveLockUnlock(void *lock) { lock_release(lock); }
int IORecursiveLockTryLock(void *lock) { return lock_try(lock); }
int IORecursiveLockHaveLock(struct runtime_lock *lock)
{
    return atomic_load(&lock->owner) == (uintptr_t)pthread_self();
}

struct thread_start {
    void (*function)(void *, int);
    void *argument;
};

static void *thread_main(void *opaque)
{
    struct thread_start start = *(struct thread_start *)opaque;
    free(opaque);
    start.function(start.argument, 0);
    return NULL;
}

int kernel_thread_start(void (*function)(void *, int), void *argument, void **out)
{
    struct thread_start *start = malloc(sizeof(*start));
    if (!start)
        return ENOMEM;
    *start = (struct thread_start){function, argument};
    pthread_t thread;
    int error = pthread_create(&thread, NULL, thread_main, start);
    if (error) {
        free(start);
        return error;
    }
    pthread_detach(thread);
    *out = (void *)thread;
    return 0;
}
void *current_thread(void) { return (void *)pthread_self(); }
void thread_deallocate(void *thread) { (void)thread; }
int thread_terminate(void *thread) { (void)thread; pthread_exit(NULL); }

struct runtime_call {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    pthread_t thread;
    void (*function)(void *, void *);
    void *argument;
    uint64_t deadline;
    bool pending, running, stopping;
};

static void *call_main(void *opaque)
{
    struct runtime_call *call = opaque;
    pthread_mutex_lock(&call->lock);
    while (!call->stopping) {
        if (!call->pending) {
            pthread_cond_wait(&call->changed, &call->lock);
            continue;
        }
        uint64_t deadline = call->deadline;
        if (mach_absolute_time() < deadline) {
            struct timespec when = {deadline / 1000000000ULL,
                                    deadline % 1000000000ULL};
            pthread_cond_timedwait(&call->changed, &call->lock, &when);
            continue;
        }
        call->pending = false;
        call->running = true;
        pthread_mutex_unlock(&call->lock);
        call->function(call->argument, NULL);
        pthread_mutex_lock(&call->lock);
        call->running = false;
        pthread_cond_broadcast(&call->changed);
    }
    pthread_mutex_unlock(&call->lock);
    return NULL;
}

void *thread_call_allocate(void (*function)(void *, void *), void *argument)
{
    struct runtime_call *call = calloc(1, sizeof(*call));
    if (!call)
        return NULL;
    call->function = function;
    call->argument = argument;
    pthread_mutex_init(&call->lock, NULL);
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&call->changed, &attr);
    pthread_condattr_destroy(&attr);
    if (pthread_create(&call->thread, NULL, call_main, call)) {
        pthread_cond_destroy(&call->changed);
        pthread_mutex_destroy(&call->lock);
        free(call);
        return NULL;
    }
    return call;
}

int thread_call_enter_delayed(struct runtime_call *call, uint64_t deadline)
{
    pthread_mutex_lock(&call->lock);
    int pending = call->pending;
    call->pending = true;
    call->deadline = deadline;
    pthread_cond_broadcast(&call->changed);
    pthread_mutex_unlock(&call->lock);
    return pending;
}
int thread_call_enter(void *call) { return thread_call_enter_delayed(call, 0); }

int thread_call_cancel(struct runtime_call *call)
{
    pthread_mutex_lock(&call->lock);
    int pending = call->pending;
    call->pending = false;
    pthread_cond_broadcast(&call->changed);
    pthread_mutex_unlock(&call->lock);
    return pending;
}

int thread_call_cancel_wait(struct runtime_call *call)
{
    pthread_mutex_lock(&call->lock);
    int pending = call->pending;
    call->pending = false;
    pthread_cond_broadcast(&call->changed);
    while (call->running)
        pthread_cond_wait(&call->changed, &call->lock);
    pthread_mutex_unlock(&call->lock);
    return pending;
}

int thread_call_free(struct runtime_call *call)
{
    if (pthread_equal(pthread_self(), call->thread))
        return 0;
    pthread_mutex_lock(&call->lock);
    call->stopping = true;
    call->pending = false;
    pthread_cond_broadcast(&call->changed);
    pthread_mutex_unlock(&call->lock);
    pthread_join(call->thread, NULL);
    pthread_cond_destroy(&call->changed);
    pthread_mutex_destroy(&call->lock);
    free(call);
    return 1;
}
