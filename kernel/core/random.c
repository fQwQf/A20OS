#include "core/random.h"
#include "core/timer.h"
#include "core/defs.h"
#include "core/lock.h"
#include "core/string.h"
#include "proc/proc.h"
#include "mm/mm.h"

static uint64_t rng_s[4];
static int rng_ready;
static uint64_t rng_generation;
static spinlock_t rng_lock = SPINLOCK_INIT;

static uint64_t splitmix64(uint64_t *x) {
    uint64_t z = (*x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static inline uint64_t rotl64(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

static uint64_t arch_entropy_sample(void) {
    uint64_t v = timer_get_ticks();
    v ^= (uint64_t)(uintptr_t)__builtin_return_address(0);
    v ^= (uint64_t)(uintptr_t)&v;
    v ^= arch_read_satp();
    v ^= frame_free_count() << 32;
    v ^= (uint64_t)(uintptr_t)proc_current();
    return v;
}

void random_reseed(uint64_t seed) {
    uint64_t x = seed ^ arch_entropy_sample();
    uint64_t flags = spin_lock_irqsave(&rng_lock);
    for (int i = 0; i < 4; i++)
        rng_s[i] ^= splitmix64(&x);
    rng_ready = 1;
    rng_generation++;
    spin_unlock_irqrestore(&rng_lock, flags);
}

void random_init(void) {
    uint64_t seed = 0xa20f00d5eed12345ULL ^ arch_entropy_sample();
    for (int i = 0; i < 4; i++)
        rng_s[i] = splitmix64(&seed);
    rng_ready = 1;
    rng_generation = 0;
}

uint64_t random_u64(void) {
    if (!rng_ready)
        random_init();

    uint64_t flags = spin_lock_irqsave(&rng_lock);

    if (++rng_generation % 64 == 0) {
        /* Inline reseed to avoid recursive lock */
        uint64_t x = arch_entropy_sample();
        for (int i = 0; i < 4; i++)
            rng_s[i] ^= splitmix64(&x);
    }

    uint64_t result = rotl64(rng_s[1] * 5, 7) * 9;
    uint64_t t = rng_s[1] << 17;

    rng_s[2] ^= rng_s[0];
    rng_s[3] ^= rng_s[1];
    rng_s[1] ^= rng_s[2];
    rng_s[0] ^= rng_s[3];
    rng_s[2] ^= t;
    rng_s[3] = rotl64(rng_s[3], 45);

    spin_unlock_irqrestore(&rng_lock, flags);
    return result;
}

void random_fill(void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    while (len > 0) {
        uint64_t r = random_u64();
        size_t n = len < sizeof(r) ? len : sizeof(r);
        memcpy(p, &r, n);
        p += n;
        len -= n;
    }
}
