/**
 * @file
 * Dynamic pool memory manager
 *
 * lwIP has dedicated pools for many structures (netconn, protocol control blocks,
 * packet buffers, ...). All these pools are managed here.
 *
 * @defgroup mempool Memory pools
 * @ingroup infrastructure
 * Custom memory pools

 */

/*
 * Copyright (c) 2001-2004 Swedish Institute of Computer Science.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 *
 * Author: Adam Dunkels <adam@sics.se>
 *
 */

#include "lwip/opt.h"

#include "lwip/memp.h"
#include "lwip/sys.h"
#include "lwip/stats.h"

#include <string.h>

/* Make sure we include everything we need for size calculation required by memp_std.h */
#include "lwip/pbuf.h"
#include "lwip/raw.h"
#include "lwip/udp.h"
#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/altcp.h"
#include "lwip/ip4_frag.h"
#include "lwip/netbuf.h"
#include "lwip/api.h"
#include "lwip/priv/tcpip_priv.h"
#include "lwip/priv/api_msg.h"
#include "lwip/priv/sockets_priv.h"
#include "lwip/etharp.h"
#include "lwip/igmp.h"
#include "lwip/timeouts.h"
/* needed by default MEMP_NUM_SYS_TIMEOUT */
#include "netif/ppp/ppp_opts.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "lwip/priv/nd6_priv.h"
#include "lwip/ip6_frag.h"
#include "lwip/mld6.h"

#define LWIP_MEMPOOL(name,num,size,desc) LWIP_MEMPOOL_DECLARE(name,num,size,desc)
#include "lwip/priv/memp_std.h"

const struct memp_desc *const memp_pools[MEMP_MAX] = {
#define LWIP_MEMPOOL(name,num,size,desc) &memp_ ## name,
#include "lwip/priv/memp_std.h"
};

#ifdef LWIP_HOOK_FILENAME
#include LWIP_HOOK_FILENAME
#endif

#ifdef LWIP_MEMP_LANE
/*
 * Per-lane memp descriptors and counters (stage C added the shape for two
 * pools, stage C2 generalised it to every pool).
 *
 * Upstream's memp has no lane dimension anywhere: memp_malloc() takes a pool
 * id, do_memp_malloc_pool() takes a descriptor, and pbuf_alloc() forwards
 * MEMP_PBUF_POOL without adding anything.  The port supplies
 * LWIP_MEMP_LANE() to answer "which lane owns the work in progress" (see
 * kernel/net/lwip_port/lwipopts.h and net_lane.h for why the answer has to be
 * the address-derived lane rather than the CPU), and this file indexes a
 * per-lane descriptor array with it.  Every pool is lane-indexed, not just the
 * two pbuf pools stage C started with: leaving TCP_PCB and friends global would
 * mean that after g_lwip_lock is split, two lanes calling tcp_pcb_alloc() would
 * still contend on -- and, on a heap-backed pool, still corrupt -- one shared
 * structure, so a partially lane-indexed memp is the one shape that is neither
 * the old global lock nor the new sharded lock.
 *
 * Lane 0 reuses the upstream descriptor object itself rather than a copy, so
 * memp_pools[] and lwip_stats.memp[] keep pointing at the very same counters
 * they always did.  Lanes 1..N-1 are copies taken at memp_init() time, which
 * is the only point where a descriptor is complete: LWIP_MEMPOOL_DECLARE
 * computes the element size from the pool's LWIP_MEMPOOL() line, and repeating
 * that expression here would be a second copy of it to keep in sync.
 *
 * WHAT THIS DOES AND DOES NOT BUY, stated plainly because it is easy to
 * over-read the name.  This is the part stage C left open and stage C2 closed,
 * and closing it did NOT produce memory partitioning:
 *
 *  - It does not partition memory.  Every profile in net_profile.h sets
 *    MEMP_MEM_MALLOC=1, under which memp_init_pool() is a stub and
 *    do_memp_malloc_pool() is mem_malloc(MEMP_SIZE + MEMP_ALIGN_SIZE(desc->size)):
 *    the descriptor contributes a size and nothing else, so two descriptors of
 *    the same size draw from the same heap.  Real partitioning needs the
 *    statically reserved layout (!MEMP_MEM_MALLOC), where each lane would need
 *    its own base array and free list -- that is not built, and the profile
 *    static asserts in lwipopts.h budget .bss for a single copy.
 *  - Freeing is unaffected either way.  do_memp_free_pool() ignores the
 *    descriptor under MEMP_MEM_MALLOC, so an element always returns to the
 *    lwIP heap no matter which lane's table the free indexed.
 *
 * So what is genuinely partitioned here is the *accounting*, and only that.
 * Two things are per-lane, and they are the only two things memp owns that
 * could be:
 *
 *   1. The descriptor table, for every pool rather than the two pbuf pools
 *      stage C started with.  Under MEMP_MEM_MALLOC the descriptor holds a
 *      size and a stats pointer, so a per-lane copy differs from its
 *      siblings only in that pointer; the table is the index a future
 *      !MEMP_MEM_MALLOC switch would hang per-lane base arrays off, and
 *      making it cover every pool now means that switch has no unpartitioned
 *      pool left to find.
 *   2. A pair of monotonic alloc/freed counters per (lane, pool).
 *
 * The descriptor's stats pointer is deliberately left pointing at the one
 * shared stats_mem, NOT at a per-lane copy.  lwip_stats.memp[] and the /proc
 * renderers read those counters as globals -- lwip_stack.c reports
 * lwip_stats.memp[MEMP_TCP_PCB]->used as "tcp_active" -- so giving each lane a
 * private stats block would silently redefine that number as "lane 0's TCP
 * PCBs" on a multi-lane build.  The counters that could be split are the ones
 * split; the ones that are read as totals stay totals.
 *
 * The counters are a pair rather than a "used" gauge.  A free carries no lane
 * -- memp_free(MEMP_PBUF, mem) has nothing but the pool id and the pointer --
 * so crediting a decrement to the freeing lane would make the gauge wrong, and
 * crediting it to the allocating lane would need a per-pbuf ownership tag,
 * which lwIP's pbuf struct has no room for.  A gauge fed that way underflows
 * its u16_t on the first lane mismatch, so it is not offered at all.  What is
 * counted here is exact and cannot underflow: how many elements each lane's
 * pool handed out, and how many came back.
 *
 * The type itself lives in memp_priv.h next to the accessor, because the port
 * renders these counters and cannot include a .c file.
 */
static struct memp_lane_count memp_lane_counts[LWIP_MEMP_LANES][MEMP_MAX];
static struct memp_desc memp_lane_desc[LWIP_MEMP_LANES - 1u][MEMP_MAX];
/* Lane 0 aliases memp_pools[] itself rather than holding a copy, so
 * memp_pools[] and lwip_stats.memp[] keep pointing at the very same objects
 * they always did.  Lanes 1..N-1 point at the copies above, which memp_init()
 * fills in -- the only point at which a descriptor is complete. */
static const struct memp_desc *memp_lane_pool[LWIP_MEMP_LANES][MEMP_MAX];

/* The three tables above are the only thing per-lane-izing memp costs, and the
 * profile budgets them.  This is the join between the two: the budget in
 * net_profile.h restates the sizes of the declarations above, and if a pool is
 * added, MEMP_MEM_MALLOC is flipped, or a table is reshaped without the budget
 * following, the tier's RAM accounting silently stops covering it.  Making that
 * a build failure instead is the whole point. */
_Static_assert(sizeof(memp_lane_counts) + sizeof(memp_lane_desc) +
                   sizeof(memp_lane_pool) ==
                   NET_PROFILE_MEMP_LANE_TABLE_BYTES,
               "the per-lane memp tables cost more than the profile's budget "
               "for them says; update NET_PROFILE_MEMP_LANE_TABLE_BYTES in "
               "net_profile.h or the tier's RAM accounting no longer covers "
               "what these tables actually allocate");

const struct memp_lane_count *
memp_lane_count_get(unsigned lane, memp_t pool)
{
  LWIP_ASSERT("memp_lane_count_get: lane out of range", lane < LWIP_MEMP_LANES);
  LWIP_ASSERT("memp_lane_count_get: pool out of range", pool < MEMP_MAX);
  if (lane >= LWIP_MEMP_LANES) {
    lane = 0;
  }
  if (pool >= MEMP_MAX) {
    pool = MEMP_PBUF;
  }
  return &memp_lane_counts[lane][pool];
}
#endif /* LWIP_MEMP_LANE */

/* The lane the port says owns this work, forced into range.  The clamp is
 * belt and braces: net_lane_ctx_push() already stores lane % LWIP_MEMP_LANES,
 * and the assert is what turns a future violation of that into a report rather
 * than an out-of-bounds read. */
#ifdef LWIP_MEMP_LANE
static u16_t
memp_lane_index(void)
{
  unsigned lane = (unsigned)LWIP_MEMP_LANE();
  if (lane >= LWIP_MEMP_LANES) {
    LWIP_ASSERT("memp: current lane out of range", 0);
    lane = 0;
  }
  return (u16_t)lane;
}

/* Every pool is indexed by lane (stage C2), so the lane a pool id resolved
 * through is just the current lane.  Kept as a function rather than inlined at
 * the two call sites so the indexing rule has one spelling. */
static u16_t
memp_pool_lane(memp_t type)
{
  LWIP_UNUSED_ARG(type);
  return memp_lane_index();
}
#endif /* LWIP_MEMP_LANE */

#ifndef LWIP_MEMP_LANE
/* One lane, one pool per id, spelled as upstream's own expression rather than
 * left to a static function that returns it.
 *
 * Both spellings compile to the same code: with the static-function form, memp.o
 * at CONFIG_NET_LANES == 1 was measured byte-identical to the pre-lane build in
 * .text, .rodata and .sdata.  So this is not a correctness fix.  It is chosen
 * because "the optimiser folds it" is a claim about one compiler version,
 * while "the preprocessed source at one lane is `memp_pools[type]`" is a
 * property anyone can check by reading the output, and the N=1 equivalence rule
 * is worth keeping checkable that way. */
#define memp_desc_for(type) memp_pools[type]
#else /* LWIP_MEMP_LANE */
/* The descriptor a pool id resolves to right now.
 *
 * A lane slot that is still NULL means memp_init() has not run for it yet.
 * Falling back to the shared descriptor is deliberate: it makes the window
 * before initialisation behave like the pre-lane stack rather than turn into a
 * NULL descriptor that memp_malloc_pool() would answer with a silent NULL. */
static const struct memp_desc *
memp_desc_for(memp_t type)
{
  const struct memp_desc *desc = memp_lane_pool[memp_lane_index()][type];
  if (desc != NULL) {
    return desc;
  }
  return memp_pools[type];
}
#endif /* LWIP_MEMP_LANE */

#if MEMP_MEM_MALLOC && MEMP_OVERFLOW_CHECK >= 2
#undef MEMP_OVERFLOW_CHECK
/* MEMP_OVERFLOW_CHECK >= 2 does not work with MEMP_MEM_MALLOC, use 1 instead */
#define MEMP_OVERFLOW_CHECK 1
#endif

#if MEMP_SANITY_CHECK && !MEMP_MEM_MALLOC
/**
 * Check that memp-lists don't form a circle, using "Floyd's cycle-finding algorithm".
 */
static int
memp_sanity(const struct memp_desc *desc)
{
  struct memp *t, *h;

  t = *desc->tab;
  if (t != NULL) {
    for (h = t->next; (t != NULL) && (h != NULL); t = t->next,
         h = ((h->next != NULL) ? h->next->next : NULL)) {
      if (t == h) {
        return 0;
      }
    }
  }

  return 1;
}
#endif /* MEMP_SANITY_CHECK && !MEMP_MEM_MALLOC */

#if MEMP_OVERFLOW_CHECK
/**
 * Check if a memp element was victim of an overflow or underflow
 * (e.g. the restricted area after/before it has been altered)
 *
 * @param p the memp element to check
 * @param desc the pool p comes from
 */
static void
memp_overflow_check_element(struct memp *p, const struct memp_desc *desc)
{
  mem_overflow_check_raw((u8_t *)p + MEMP_SIZE, desc->size, "pool ", desc->desc);
}

/**
 * Initialize the restricted area of on memp element.
 */
static void
memp_overflow_init_element(struct memp *p, const struct memp_desc *desc)
{
  mem_overflow_init_raw((u8_t *)p + MEMP_SIZE, desc->size);
}

#if MEMP_OVERFLOW_CHECK >= 2
/**
 * Do an overflow check for all elements in every pool.
 *
 * @see memp_overflow_check_element for a description of the check
 */
static void
memp_overflow_check_all(void)
{
  u16_t i, j;
  struct memp *p;
  SYS_ARCH_DECL_PROTECT(old_level);
  SYS_ARCH_PROTECT(old_level);

  for (i = 0; i < MEMP_MAX; ++i) {
    p = (struct memp *)LWIP_MEM_ALIGN(memp_pools[i]->base);
    for (j = 0; j < memp_pools[i]->num; ++j) {
      memp_overflow_check_element(p, memp_pools[i]);
      p = LWIP_ALIGNMENT_CAST(struct memp *, ((u8_t *)p + MEMP_SIZE + memp_pools[i]->size + MEM_SANITY_REGION_AFTER_ALIGNED));
    }
  }
  SYS_ARCH_UNPROTECT(old_level);
}
#endif /* MEMP_OVERFLOW_CHECK >= 2 */
#endif /* MEMP_OVERFLOW_CHECK */

/**
 * Initialize custom memory pool.
 * Related functions: memp_malloc_pool, memp_free_pool
 *
 * @param desc pool to initialize
 */
void
memp_init_pool(const struct memp_desc *desc)
{
#if MEMP_MEM_MALLOC
  LWIP_UNUSED_ARG(desc);
#else
  int i;
  struct memp *memp;

  *desc->tab = NULL;
  memp = (struct memp *)LWIP_MEM_ALIGN(desc->base);
#if MEMP_MEM_INIT
  /* force memset on pool memory */
  memset(memp, 0, (size_t)desc->num * (MEMP_SIZE + desc->size
#if MEMP_OVERFLOW_CHECK
                                       + MEM_SANITY_REGION_AFTER_ALIGNED
#endif
                                      ));
#endif
  /* create a linked list of memp elements */
  for (i = 0; i < desc->num; ++i) {
    memp->next = *desc->tab;
    *desc->tab = memp;
#if MEMP_OVERFLOW_CHECK
    memp_overflow_init_element(memp, desc);
#endif /* MEMP_OVERFLOW_CHECK */
    /* cast through void* to get rid of alignment warnings */
    memp = (struct memp *)(void *)((u8_t *)memp + MEMP_SIZE + desc->size
#if MEMP_OVERFLOW_CHECK
                                   + MEM_SANITY_REGION_AFTER_ALIGNED
#endif
                                  );
  }
#if MEMP_STATS
  desc->stats->avail = desc->num;
#endif /* MEMP_STATS */
#endif /* !MEMP_MEM_MALLOC */

#if MEMP_STATS && (defined(LWIP_DEBUG) || LWIP_STATS_DISPLAY)
  desc->stats->name  = desc->desc;
#endif /* MEMP_STATS && (defined(LWIP_DEBUG) || LWIP_STATS_DISPLAY) */
}

/**
 * Initializes lwIP built-in pools.
 * Related functions: memp_malloc, memp_free
 *
 * Carves out memp_memory into linked lists for each pool-type.
 */
void
memp_init(void)
{
  u16_t i;

  /* for every pool: */
  for (i = 0; i < LWIP_ARRAYSIZE(memp_pools); i++) {
    memp_init_pool(memp_pools[i]);

#if LWIP_STATS && MEMP_STATS
    lwip_stats.memp[i] = memp_pools[i]->stats;
#endif
  }

#ifdef LWIP_MEMP_LANE
  /* Take the per-lane copies now, from the descriptors memp_init_pool() has
   * just finished with -- the only point at which a descriptor is complete,
   * because LWIP_MEMPOOL_DECLARE computes its element size from the pool's
   * LWIP_MEMPOOL() line and repeating that expression here would be a second
   * copy of it to keep in sync.
   *
   * Every pool is copied, not just the two pbuf pools stage C started with.
   * Lane 0 aliases memp_pools[] itself, so memp_pools[] and lwip_stats.memp[]
   * are untouched for it and the aggregate counters keep their old meaning.
   *
   * The copies keep the shared stats_mem pointer on purpose; see the note
   * above memp_lane_counts. */
  for (i = 0; i < MEMP_MAX; i++) {
    memp_lane_pool[0][i] = memp_pools[i];
  }
  {
    u16_t lane;
    for (lane = 1; lane < LWIP_MEMP_LANES; lane++) {
      for (i = 0; i < MEMP_MAX; i++) {
        memp_lane_desc[lane - 1u][i] = *memp_pools[i];
        memp_lane_pool[lane][i] = &memp_lane_desc[lane - 1u][i];
      }
    }
  }
#endif /* LWIP_MEMP_LANE */

#if MEMP_OVERFLOW_CHECK >= 2
  /* check everything a first time to see if it worked */
  memp_overflow_check_all();
#endif /* MEMP_OVERFLOW_CHECK >= 2 */
}

static void *
#if !MEMP_OVERFLOW_CHECK
do_memp_malloc_pool(const struct memp_desc *desc)
#else
do_memp_malloc_pool_fn(const struct memp_desc *desc, const char *file, const int line)
#endif
{
  struct memp *memp;
  SYS_ARCH_DECL_PROTECT(old_level);

#if MEMP_MEM_MALLOC
  memp = (struct memp *)mem_malloc(MEMP_SIZE + MEMP_ALIGN_SIZE(desc->size));
  SYS_ARCH_PROTECT(old_level);
#else /* MEMP_MEM_MALLOC */
  SYS_ARCH_PROTECT(old_level);

  memp = *desc->tab;
#endif /* MEMP_MEM_MALLOC */

  if (memp != NULL) {
#if !MEMP_MEM_MALLOC
#if MEMP_OVERFLOW_CHECK == 1
    memp_overflow_check_element(memp, desc);
#endif /* MEMP_OVERFLOW_CHECK */

    *desc->tab = memp->next;
#if MEMP_OVERFLOW_CHECK
    memp->next = NULL;
#endif /* MEMP_OVERFLOW_CHECK */
#endif /* !MEMP_MEM_MALLOC */
#if MEMP_OVERFLOW_CHECK
    memp->file = file;
    memp->line = line;
#if MEMP_MEM_MALLOC
    memp_overflow_init_element(memp, desc);
#endif /* MEMP_MEM_MALLOC */
#endif /* MEMP_OVERFLOW_CHECK */
    LWIP_ASSERT("memp_malloc: memp properly aligned",
                ((mem_ptr_t)memp % MEM_ALIGNMENT) == 0);
#if MEMP_STATS
    desc->stats->used++;
    if (desc->stats->used > desc->stats->max) {
      desc->stats->max = desc->stats->used;
    }
#endif
    SYS_ARCH_UNPROTECT(old_level);
    /* cast through u8_t* to get rid of alignment warnings */
    return ((u8_t *)memp + MEMP_SIZE);
  } else {
#if MEMP_STATS
    desc->stats->err++;
#endif
    SYS_ARCH_UNPROTECT(old_level);
    LWIP_DEBUGF(MEMP_DEBUG | LWIP_DBG_LEVEL_SERIOUS, ("memp_malloc: out of memory in pool %s\n", desc->desc));
  }

  return NULL;
}

/**
 * Get an element from a custom pool.
 *
 * @param desc the pool to get an element from
 *
 * @return a pointer to the allocated memory or a NULL pointer on error
 */
void *
#if !MEMP_OVERFLOW_CHECK
memp_malloc_pool(const struct memp_desc *desc)
#else
memp_malloc_pool_fn(const struct memp_desc *desc, const char *file, const int line)
#endif
{
  LWIP_ASSERT("invalid pool desc", desc != NULL);
  if (desc == NULL) {
    return NULL;
  }

#if !MEMP_OVERFLOW_CHECK
  return do_memp_malloc_pool(desc);
#else
  return do_memp_malloc_pool_fn(desc, file, line);
#endif
}

/**
 * Get an element from a specific pool.
 *
 * @param type the pool to get an element from
 *
 * @return a pointer to the allocated memory or a NULL pointer on error
 */
void *
#if !MEMP_OVERFLOW_CHECK
memp_malloc(memp_t type)
#else
memp_malloc_fn(memp_t type, const char *file, const int line)
#endif
{
  void *memp;
  LWIP_ERROR("memp_malloc: type < MEMP_MAX", (type < MEMP_MAX), return NULL;);

#if MEMP_OVERFLOW_CHECK >= 2
  memp_overflow_check_all();
#endif /* MEMP_OVERFLOW_CHECK >= 2 */

#if !MEMP_OVERFLOW_CHECK
  memp = do_memp_malloc_pool(memp_desc_for(type));
#else
  memp = do_memp_malloc_pool_fn(memp_desc_for(type), file, line);
#endif

#ifdef LWIP_MEMP_LANE
  /* Counted after the allocation, so a failure does not inflate the lane's
   * share and a reader can tell a lane that was refused from one that never
   * asked.  u32_t wraps only after 4G elements, and it only ever increments. */
  if (memp != NULL) {
    SYS_ARCH_LOCKED(memp_lane_counts[memp_pool_lane(type)][type].alloc++);
  }
#endif /* LWIP_MEMP_LANE */

  return memp;
}

static void
do_memp_free_pool(const struct memp_desc *desc, void *mem)
{
  struct memp *memp;
  SYS_ARCH_DECL_PROTECT(old_level);

  LWIP_ASSERT("memp_free: mem properly aligned",
              ((mem_ptr_t)mem % MEM_ALIGNMENT) == 0);

  /* cast through void* to get rid of alignment warnings */
  memp = (struct memp *)(void *)((u8_t *)mem - MEMP_SIZE);

  SYS_ARCH_PROTECT(old_level);

#if MEMP_OVERFLOW_CHECK == 1
  memp_overflow_check_element(memp, desc);
#endif /* MEMP_OVERFLOW_CHECK */

#if MEMP_STATS
  desc->stats->used--;
#endif

#if MEMP_MEM_MALLOC
  LWIP_UNUSED_ARG(desc);
  SYS_ARCH_UNPROTECT(old_level);
  mem_free(memp);
#else /* MEMP_MEM_MALLOC */
  memp->next = *desc->tab;
  *desc->tab = memp;

#if MEMP_SANITY_CHECK
  LWIP_ASSERT("memp sanity", memp_sanity(desc));
#endif /* MEMP_SANITY_CHECK */

  SYS_ARCH_UNPROTECT(old_level);
#endif /* !MEMP_MEM_MALLOC */
}

/**
 * Put a custom pool element back into its pool.
 *
 * @param desc the pool where to put mem
 * @param mem the memp element to free
 */
void
memp_free_pool(const struct memp_desc *desc, void *mem)
{
  LWIP_ASSERT("invalid pool desc", desc != NULL);
  if ((desc == NULL) || (mem == NULL)) {
    return;
  }

  do_memp_free_pool(desc, mem);
}

/**
 * Put an element back into its pool.
 *
 * @param type the pool where to put mem
 * @param mem the memp element to free
 */
void
memp_free(memp_t type, void *mem)
{
#ifdef LWIP_HOOK_MEMP_AVAILABLE
  struct memp *old_first;
#endif

  LWIP_ERROR("memp_free: type < MEMP_MAX", (type < MEMP_MAX), return;);

  if (mem == NULL) {
    return;
  }

#if MEMP_OVERFLOW_CHECK >= 2
  memp_overflow_check_all();
#endif /* MEMP_OVERFLOW_CHECK >= 2 */

#ifdef LWIP_HOOK_MEMP_AVAILABLE
  old_first = *memp_desc_for(type)->tab;
#endif

  do_memp_free_pool(memp_desc_for(type), mem);

#ifdef LWIP_MEMP_LANE
  /*
   * Counted here, against the freeing lane.  It is deliberately not a "used"
   * gauge: memp_free() is handed a pool id and a pointer and nothing that says
   * which lane's pool handed the element out, so a decrement attributed to the
   * freeing lane would drift, and one attributed to the allocating lane would
   * need a per-element tag lwIP has nowhere to put.  Two monotonic counters
   * are exact and cannot underflow; see the note on memp_lane_count above.
   */
  SYS_ARCH_LOCKED(memp_lane_counts[memp_pool_lane(type)][type].freed++);
#endif /* LWIP_MEMP_LANE */

#ifdef LWIP_HOOK_MEMP_AVAILABLE
  if (old_first == NULL) {
    LWIP_HOOK_MEMP_AVAILABLE(type);
  }
#endif
}
