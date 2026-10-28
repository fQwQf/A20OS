#ifndef A20_LWIP_ARCH_HEAP_H
#define A20_LWIP_ARCH_HEAP_H

/* Shared with the host concurrency harness so it tests the production gate. */
void a20_lwip_heap_lock(void);
void a20_lwip_heap_unlock(void);
#define LWIP_HEAP_LOCK() a20_lwip_heap_lock()
#define LWIP_HEAP_UNLOCK() a20_lwip_heap_unlock()

#endif
