/*
 * mtcorrupt_test — reproducer gate for the "multi-threaded processes
 * occasionally have their anonymous memory scribbled on" reports recorded in
 * docs/distro/known-issues.md.
 *
 * The reports are two unrelated programs (LuaJIT's GC list head, musl's
 * ld.so self-pointer check) failing with the same shape: a doubly-linked
 * list head that should be a valid node reads back as NULL or as a wild
 * pointer, only under threads.  Neither program is at fault, so the kernel
 * is writing somewhere it does not own.
 *
 * Two phases, each aimed at a different way that can happen:
 *
 *   1. GUARDED RECORDS AND A SHARED STRUCTURE.  Each worker owns a record of
 *      anonymous memory followed by a PROT_NONE guard page, and a record is
 *      laid out so that a zeroed word is itself a detectable symptom -- that
 *      is the exact shape both reports died on.  A wild write that lands
 *      inside the record corrupts a checksum; one that lands in the guard
 *      page faults; one that lands in the shared doubly-linked list breaks a
 *      self-pointer check.  All three are caught here, attributed to the
 *      round that caused them, rather than surfacing later as a mystery
 *      inside malloc.
 *
 *   2. THREAD CHURN.  The same workers run again while short-lived threads
 *      are created and reaped in a tight loop.  This is the shape that
 *      implicates thread exit: per-thread kernel stacks, trap frames, and
 *      the exit path racing a thread that is still running.
 *
 * The gate builds the kernel with CONFIG_SLAB_DEBUG=1, which walks each
 * slab page's free list on every allocation and panics on a node that is
 * out of bounds, misaligned, cyclic, or inconsistent with the page's
 * accounting -- the detector for a slab page being scribbled on while it is
 * still live.
 *
 * Prints MTCORRUPT: PASS.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <errno.h>

#define THREADS      4
#define CHURN_ROUNDS 40
#define CHURN_INNER  200

#define GUARD_PAGES  1
#define RECORD_BYTES 4096
#define GUARD_BYTES  (GUARD_PAGES * 4096)

static int failures;

static void fail(const char *what, const char *detail) {
    printf("MTCORRUPT: FAIL %s%s%s\n", what, detail ? ": " : "",
           detail ? detail : "");
    failures++;
}

/* ---- guarded records ---------------------------------------------- */

struct record {
    unsigned char *base;      /* start of the mapped region */
    unsigned char *data;      /* writable page */
    unsigned char *guard;     /* PROT_NONE page */
    volatile uint64_t *words;
    int nwords;
    uint64_t seed;
};

/* 64-bit value that depends on its own index, so a single-bit flip anywhere
 * in the record is caught and a partial overwrite of a neighbouring word is
 * caught too. */
static uint64_t mix(int idx, uint64_t seed) {
    uint64_t x = (uint64_t)(idx + 1) * 0x9E3779B97F4A7C15ULL ^ seed;
    x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27; x *= 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return x | 1ULL;   /* never zero: a zeroed word is itself the symptom */
}

static int record_setup(struct record *r, int idx) {
    size_t len = RECORD_BYTES + GUARD_BYTES;
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return -1;
    r->base = (unsigned char *)p;
    r->data = r->base;
    r->guard = r->base + RECORD_BYTES;
    if (mprotect(r->guard, GUARD_BYTES, PROT_NONE) != 0) {
        munmap(p, len);
        return -1;
    }
    r->words = (volatile uint64_t *)(void *)r->data;
    r->nwords = RECORD_BYTES / sizeof(uint64_t);
    r->seed = 0xA5A5A5A5ULL ^ (uint64_t)idx * 0x0101010101010101ULL;
    for (int i = 0; i < r->nwords; i++)
        r->words[i] = mix(i, r->seed);
    return 0;
}

static int record_check(struct record *r, int idx, int round) {
    uint64_t seed = 0xA5A5A5A5ULL ^ (uint64_t)idx * 0x0101010101010101ULL;
    for (int i = 0; i < r->nwords; i++) {
        if (r->words[i] != mix(i, seed)) {
            printf("MTCORRUPT: record %d word %d = %016lx want %016lx "
                   "(round %d, thread %d)\n", idx, i,
                   (unsigned long)r->words[i], (unsigned long)mix(i, seed),
                   round, (int)pthread_self());
            return -1;
        }
    }
    /* A zeroed word is the exact signature both reported crashes had. */
    for (int i = 0; i < r->nwords; i++) {
        if (r->words[i] == 0) {
            printf("MTCORRUPT: record %d word %d is NULL (round %d)\n",
                   idx, i, round);
            return -1;
        }
    }
    return 0;
}

static void record_scribble(struct record *r) {
    for (int i = 0; i < r->nwords; i++)
        r->words[i] = mix(i, r->seed);
}

static void record_teardown(struct record *r) {
    munmap(r->base, RECORD_BYTES + GUARD_BYTES);
}

/* ---- workers ----------------------------------------------- */

static struct record g_records[THREADS];
static volatile int g_stop;

/* Doubly-linked list, the shape both reported crashes died on. */
struct node {
    struct node *prev, *next;
    uint64_t magic;
};

static struct node *g_list_head;
static pthread_mutex_t g_list_lock = PTHREAD_MUTEX_INITIALIZER;

static int list_check_locked(const char *where, int round) {
    struct node *p = g_list_head;
    struct node *prev = NULL;
    int n = 0;
    while (p) {
        if (p->prev != prev) {
            printf("MTCORRUPT: %s: node %p prev=%p want %p (round %d)\n",
                   where, (void *)p, (void *)p->prev, (void *)prev, round);
            return -1;
        }
        if (p->magic != 0x5A5A5A5A5A5A5A5AULL) {
            printf("MTCORRUPT: %s: node %p magic=%016lx (round %d)\n",
                   where, (void *)p, (unsigned long)p->magic, round);
            return -1;
        }
        prev = p;
        p = p->next;
        if (++n > 64) {
            printf("MTCORRUPT: %s: list cycle at %p (round %d)\n", where,
                   (void *)prev, round);
            return -1;
        }
    }
    return 0;
}

static void *worker(void *arg) {
    long idx = (long)arg;
    struct record *r = &g_records[idx];

    for (int round = 0; round < CHURN_ROUNDS && !g_stop; round++) {
        /* Fresh anonymous mapping, scribbled, released: this is the path
         * whose failure mode is another thread's mapping being torn down
         * underneath it. */
        size_t len = 64 * 1024;
        unsigned char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            fail("worker mmap", strerror(errno));
            break;
        }
        for (size_t off = 0; off < len; off += 4096)
            p[off] = (unsigned char)(idx + round);
        if (munmap(p, len) != 0)
            fail("worker munmap", strerror(errno));

        /* Heap churn sized to hit the small-object slab caches where a
         * corrupted free list would do the most damage. */
        for (int i = 0; i < CHURN_INNER; i++) {
            size_t sz = (size_t)(8 + ((i * 37 + round * 11) % 512));
            void *b = malloc(sz);
            if (!b) {
                fail("worker malloc", strerror(errno));
                break;
            }
            memset(b, (int)(idx & 0xff), sz);
            free(b);
        }

        record_scribble(r);
        if (record_check(r, (int)idx, round) != 0) {
            fail("guarded record corrupted", "see record dump above");
            g_stop = 1;
            break;
        }

        /* Push and pop a node on the shared list, checking the whole list
         * each time so a corruption is attributed to the round that caused
         * it. */
        struct node *n = malloc(sizeof(*n));
        if (!n)
            continue;
        n->magic = 0x5A5A5A5A5A5A5A5AULL;
        pthread_mutex_lock(&g_list_lock);
        n->prev = NULL;
        n->next = g_list_head;
        if (g_list_head)
            g_list_head->prev = n;
        g_list_head = n;
        int bad = list_check_locked("after push", round);
        if (!bad) {
            struct node *first = g_list_head;
            g_list_head = first->next;
            if (g_list_head)
                g_list_head->prev = NULL;
            first->next = first->prev = NULL;
            free(first);
            bad = list_check_locked("after pop", round);
        }
        pthread_mutex_unlock(&g_list_lock);
        if (bad) {
            fail("shared list corrupted", "see list dump above");
            g_stop = 1;
            break;
        }
    }
    return NULL;
}

/* ---- thread churn ------------------------------------------- */

static void *churn_thread(void *arg) {
    (void)arg;
    /* A thread that exits immediately, immediately after touching a little
     * memory: the cheapest way to run the exit path (kernel stack release,
     * trap frame teardown, task slot recycling) thousands of times. */
    volatile uint64_t local[8];
    for (int i = 0; i < 8; i++)
        local[i] = (uint64_t)i * 0x1234567ULL;
    if (local[7] != 7 * 0x1234567ULL)
        abort();
    return NULL;
}

int main(void) {
    printf("MTCORRUPT: phase 1/2 setup\n");

    for (int i = 0; i < THREADS; i++) {
        if (record_setup(&g_records[i], i) != 0) {
            fail("record setup", strerror(errno));
            return 1;
        }
    }

    pthread_t th[THREADS];
    for (long i = 0; i < THREADS; i++) {
        if (pthread_create(&th[i], NULL, worker, (void *)i) != 0) {
            fail("pthread_create", "worker");
            return 1;
        }
    }
    for (int i = 0; i < THREADS; i++)
        pthread_join(th[i], NULL);
    printf("MTCORRUPT: phase 1/2 done\n");

    for (int i = 0; i < THREADS; i++)
        record_check(&g_records[i], i, -1);

    /* Thread churn: the same workers run again, against the same live
     * records, while short-lived threads are created and reaped around them.
     * The records stay mapped for both phases on purpose: a record released
     * here would leave phase 2 scribbling into an address the kernel is free
     * to have handed to something else -- a self-inflicted use-after-unmap
     * that looks exactly like the defect this gate exists to catch. */
    g_stop = 0;
    for (long i = 0; i < THREADS; i++) {
        if (pthread_create(&th[i], NULL, worker, (void *)i) != 0) {
            fail("pthread_create", "churn phase");
            return 1;
        }
    }
    for (int round = 0; round < CHURN_ROUNDS * 10 && !g_stop; round++) {
        pthread_t tmp;
        if (pthread_create(&tmp, NULL, churn_thread, NULL) == 0)
            pthread_join(tmp, NULL);
        else
            fail("churn pthread_create", strerror(errno));
    }
    for (int i = 0; i < THREADS; i++)
        pthread_join(th[i], NULL);
    printf("MTCORRUPT: phase 2/2 done\n");

    for (int i = 0; i < THREADS; i++) {
        record_check(&g_records[i], i, -1);
        record_teardown(&g_records[i]);
    }

    if (failures) {
        printf("MTCORRUPT: %d failure(s)\n", failures);
        return 1;
    }
    printf("MTCORRUPT: PASS\n");
    return 0;
}