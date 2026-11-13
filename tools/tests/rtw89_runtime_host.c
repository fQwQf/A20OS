/* SPDX-License-Identifier: BSD-3-Clause */
#include "../../user/wifi/runtime.c"
#include <assert.h>

struct check {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    unsigned calls;
    bool hold, entered;
};

static void callback(void *argument, void *unused)
{
    (void)unused;
    struct check *check = argument;
    pthread_mutex_lock(&check->mutex);
    check->entered = true;
    check->calls++;
    pthread_cond_broadcast(&check->changed);
    while (check->hold)
        pthread_cond_wait(&check->changed, &check->mutex);
    pthread_mutex_unlock(&check->mutex);
}

static void await_callback(struct check *check)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 3;
    pthread_mutex_lock(&check->mutex);
    while (!check->entered)
        assert(!pthread_cond_timedwait(&check->changed, &check->mutex, &deadline));
    pthread_mutex_unlock(&check->mutex);
}

struct free_check { struct runtime_call *call; atomic_bool done; };
static void *free_call(void *argument)
{
    struct free_check *check = argument;
    assert(thread_call_free(check->call));
    atomic_store(&check->done, true);
    return NULL;
}

static void *check_other_owner(void *lock)
{
    assert(!IORecursiveLockHaveLock(lock));
    assert(!IORecursiveLockTryLock(lock));
    return NULL;
}

int main(void)
{
    struct check check = {.mutex = PTHREAD_MUTEX_INITIALIZER,
                          .changed = PTHREAD_COND_INITIALIZER};
    struct runtime_call *call = thread_call_allocate(callback, &check);
    assert(call);
    assert(!thread_call_enter_delayed(call, mach_absolute_time() + 5000000000ULL));
    assert(thread_call_cancel_wait(call));
    assert(check.calls == 0);
    assert(!thread_call_enter_delayed(call, mach_absolute_time() + 5000000000ULL));
    assert(thread_call_enter_delayed(call, mach_absolute_time() + 10000000ULL));
    await_callback(&check);
    thread_call_cancel_wait(call);
    assert(check.calls == 1);
    assert(thread_call_free(call));

    check.hold = true;
    check.entered = false;
    call = thread_call_allocate(callback, &check);
    assert(call && !thread_call_enter(call));
    await_callback(&check);
    struct free_check freeing = {.call = call};
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, free_call, &freeing));
    IOSleep(20);
    assert(!atomic_load(&freeing.done));
    pthread_mutex_lock(&check.mutex);
    check.hold = false;
    pthread_cond_broadcast(&check.changed);
    pthread_mutex_unlock(&check.mutex);
    assert(!pthread_join(thread, NULL));
    assert(atomic_load(&freeing.done) && check.calls == 2);

    void *lock = IORecursiveLockAlloc();
    assert(lock && !IORecursiveLockHaveLock(lock));
    IORecursiveLockLock(lock);
    IORecursiveLockLock(lock);
    assert(IORecursiveLockHaveLock(lock));
    assert(!pthread_create(&thread, NULL, check_other_owner, lock));
    assert(!pthread_join(thread, NULL));
    IORecursiveLockUnlock(lock);
    assert(IORecursiveLockHaveLock(lock));
    IORecursiveLockUnlock(lock);
    assert(!IORecursiveLockHaveLock(lock));
    IORecursiveLockFree(lock);
    puts("rtw89 runtime: cancel, retime, drain/free and recursive ownership PASS");
    return 0;
}
