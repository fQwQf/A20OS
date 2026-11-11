#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.c"

_Thread_local unsigned host_lock_depth;
static unsigned allocations, hits, shared_hits;
void *kmalloc(size_t n)
{ assert(!host_lock_depth); void *p = malloc(n); assert(p); allocations++; return p; }
void kfree(void *p)
{ assert(!host_lock_depth); if (p) { assert(allocations); allocations--; free(p); } }
struct gate { pthread_mutex_t lock; pthread_cond_t cond; int entered, release; };
#define GATE_INIT {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0}
static struct gate handler_gate, eoi_gate;
static int block_handler, block_eoi;
static unsigned enabled;
static void enter(struct gate *g)
{
    pthread_mutex_lock(&g->lock); g->entered = 1; pthread_cond_broadcast(&g->cond);
    while (!g->release) pthread_cond_wait(&g->cond, &g->lock);
    pthread_mutex_unlock(&g->lock);
}
static void wait_enter(struct gate *g)
{
    pthread_mutex_lock(&g->lock);
    while (!g->entered) pthread_cond_wait(&g->cond, &g->lock);
    pthread_mutex_unlock(&g->lock);
}
static void release(struct gate *g)
{
    pthread_mutex_lock(&g->lock); g->release = 1;
    pthread_cond_broadcast(&g->cond); pthread_mutex_unlock(&g->lock);
}
static void enable_line(uint32_t irq) { assert(irq == 42); enabled = 1; }
static void disable_line(uint32_t irq) { assert(irq == 42); enabled = 0; }
static void eoi_line(uint32_t irq) { assert(irq == 42); if (block_eoi) enter(&eoi_gate); }
static const irqchip_ops_t chip = {.enable_irq=enable_line, .disable_irq=disable_line, .eoi=eoi_line};
static const board_config_t board = {.irqchip=&chip};
const board_config_t *const current_board = &board;
static int handler(int irq, void *p)
{
    assert(irq == 42 && p == &hits); hits++;
    if (block_handler)
        enter(&handler_gate);
    return 0;
}
static int secondary(int irq, void *p)
{ assert(irq == 42 && p == &shared_hits); shared_hits++; return 0; }
static void *dispatch_thread(void *unused)
{ (void)unused; driver_irq_dispatch(42); return NULL; }
struct waiter { int free, done; void *priv; };
static void *wait_thread(void *p)
{
    struct waiter *w = p;
    if (w->free) free_irq(42, w->priv);
    else assert(!synchronize_irq(42));
    __atomic_store_n(&w->done, 1, __ATOMIC_RELEASE); return NULL;
}
static void setup(void)
{
    handler_gate = (struct gate)GATE_INIT;
    eoi_gate = (struct gate)GATE_INIT;
    hits = shared_hits = 0; block_handler = block_eoi = 1;
}
static void complete_dispatch_is_drained(int free_handler)
{
    setup(); assert(!request_irq(42, handler, 0, &hits));
    pthread_t dispatch, waiter;
    pthread_create(&dispatch, NULL, dispatch_thread, NULL); wait_enter(&handler_gate);
    struct waiter w = {free_handler, 0, &hits};
    pthread_create(&waiter, NULL, wait_thread, &w);
    assert(__atomic_load_n(&irq_active[42], __ATOMIC_ACQUIRE) == 1);
    assert(!__atomic_load_n(&w.done, __ATOMIC_ACQUIRE));
    release(&handler_gate); wait_enter(&eoi_gate);
    /* EOI is still part of dispatch lifetime, even though the handler returned. */
    assert(__atomic_load_n(&irq_active[42], __ATOMIC_ACQUIRE) == 1);
    assert(!__atomic_load_n(&w.done, __ATOMIC_ACQUIRE));
    release(&eoi_gate); pthread_join(dispatch, NULL); pthread_join(waiter, NULL);
    assert(w.done && !irq_active[42]);
    if (!free_handler) {
        block_handler = block_eoi = 0; driver_irq_dispatch(42); assert(hits == 2);
        free_irq(42, &hits);
    }
    assert(!irq_handlers[42] && !enabled);
}
static void shared_promotion_and_unlink(void)
{
    setup();
    assert(!request_irq(42, handler, IRQF_SHARED, &hits));
    assert(!request_irq(42, secondary, IRQF_SHARED | IRQF_NO_AUTO_ENABLE, &shared_hits));
    pthread_t dispatch, waiter;
    pthread_create(&dispatch, NULL, dispatch_thread, NULL); wait_enter(&handler_gate);
    struct waiter w = {1, 0, &shared_hits};
    pthread_create(&waiter, NULL, wait_thread, &w);
    assert(!__atomic_load_n(&w.done, __ATOMIC_ACQUIRE) && allocations == 1);
    release(&handler_gate); wait_enter(&eoi_gate);
    assert(shared_hits == 1 && !__atomic_load_n(&w.done, __ATOMIC_ACQUIRE));
    release(&eoi_gate); pthread_join(dispatch, NULL); pthread_join(waiter, NULL);
    assert(!allocations && irq_handlers[42] == handler);
    block_handler = block_eoi = 0;
    assert(!request_irq(42, secondary, IRQF_SHARED | IRQF_NO_AUTO_ENABLE, &shared_hits));
    free_irq(42, &hits);
    assert(irq_handlers[42] == secondary);
    assert(irq_flags[42] == (IRQF_SHARED | IRQF_NO_AUTO_ENABLE));
    free_irq(42, &hits); assert(irq_handlers[42] == secondary); /* Wrong owner. */
    free_irq(42, &shared_hits); assert(!allocations && !enabled);
}
int main(void)
{
    assert(synchronize_irq(256) == -EINVAL && !synchronize_irq(42));
    complete_dispatch_is_drained(0); complete_dispatch_is_drained(1);
    shared_promotion_and_unlink();
    puts("IRQ complete-dispatch synchronization: PASS"); return 0;
}
