#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include "fs/vfs/shutdown.h"

typedef struct trace {
    int sequence[16];
    int used;
    int sync_calls;
    int teardown_calls;
    int commit_calls;
    int reenter;
    int reenter_result;
    const vfs_shutdown_ops_t *ops;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int block_sync;
    int sync_entered;
    int release_sync;
    int first_result;
} trace_t;

static int record(trace_t *t, int phase)
{
    assert(t->used < (int)(sizeof(t->sequence) / sizeof(t->sequence[0])));
    t->sequence[t->used++] = phase;
    return 0;
}

static int begin(void *ctx) { return record(ctx, 1); }
static int sync_cb(void *ctx)
{
    trace_t *t = ctx;
    pthread_mutex_lock(&t->lock);
    record(t, 2);
    t->sync_calls++;
    int call = t->sync_calls;
    if (t->block_sync && call == 1) {
        t->sync_entered = 1;
        pthread_cond_broadcast(&t->cond);
        while (!t->release_sync)
            pthread_cond_wait(&t->cond, &t->lock);
    }
    pthread_mutex_unlock(&t->lock);
    if (t->reenter) {
        t->reenter = 0;
        t->reenter_result = a20_vfs_shutdown_run(t->ops);
    }
    return call == 1 ? -5 : 0;
}
static int teardown(void *ctx)
{
    trace_t *t = ctx;
    record(t, 3);
    t->teardown_calls++;
    return t->teardown_calls == 1 ? -5 : 0;
}
static int abort_cb(void *ctx) { return record(ctx, 4) ?: -16; }
static int commit(void *ctx)
{
    trace_t *t = ctx;
    record(t, 5);
    t->commit_calls++;
    return t->commit_calls == 1 ? -16 : 0;
}

static void *run_first(void *opaque)
{
    trace_t *t = opaque;
    t->first_result = a20_vfs_shutdown_run(t->ops);
    return NULL;
}

int main(void)
{
    trace_t t;
    memset(&t, 0, sizeof(t));
    pthread_mutex_init(&t.lock, NULL);
    pthread_cond_init(&t.cond, NULL);
    t.block_sync = 1;
    vfs_shutdown_ops_t ops = {
        .context = &t,
        .begin = begin,
        .sync = sync_cb,
        .teardown = teardown,
        .abort = abort_cb,
        .commit = commit,
    };
    t.ops = &ops;

    pthread_t worker;
    assert(pthread_create(&worker, NULL, run_first, &t) == 0);
    pthread_mutex_lock(&t.lock);
    while (!t.sync_entered)
        pthread_cond_wait(&t.cond, &t.lock);
    pthread_mutex_unlock(&t.lock);
    assert(a20_vfs_shutdown_run(&ops) == -16);
    pthread_mutex_lock(&t.lock);
    t.release_sync = 1;
    pthread_cond_broadcast(&t.cond);
    pthread_mutex_unlock(&t.lock);
    assert(pthread_join(worker, NULL) == 0);
    assert(t.first_result == -5);
    t.block_sync = 0;
    assert(t.used == 3 && t.sequence[0] == 1 && t.sequence[1] == 2 &&
           t.sequence[2] == 4);
    assert(a20_vfs_shutdown_run(&ops) == -16);
    assert(t.used == 5 && t.sequence[3] == 2 && t.sequence[4] == 5);
    assert(a20_vfs_shutdown_run(&ops) == -5);
    assert(t.used == 7 && t.sequence[5] == 5 && t.sequence[6] == 3);
    assert(a20_vfs_shutdown_run(&ops) == 0);
    assert(t.used == 8 && t.sequence[7] == 3);
    assert(a20_vfs_shutdown_run(&ops) == 0 && t.used == 8);
    assert(t.sync_calls == 2 && t.commit_calls == 2 && t.teardown_calls == 2);
    return 0;
}
