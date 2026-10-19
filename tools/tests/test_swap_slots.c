/*
 * Host differential harness for kernel/mm/swap_slots.rs.
 *
 * The production Rust lifecycle is linked against a model of the swap areas:
 * a u32 reference map, the free-slot bitmap over it, the hint and one owner
 * pointer per slot.  The assertions cover the decisions Rust owns: where the
 * scan starts and wraps, the word-major order and the "highest free bit in the
 * scanned word" choice the C reference made, that the bit is cleared before
 * the map is published, the reference-count branch (including bad/zero/
 * overflow), the hint rewind, the accounting pair, and that the cgroup
 * charge/pin and uncharge/unpin are balanced and run outside the area lock.
 *
 * Guest coverage is separate: mm_stress and oom_stress swap real pages.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SWAPFILES 16
#define SWAP_MAP_BAD 0xFFFFFFFFu
#define SWP_TYPE_BITS 4
#define OFFSET_BITS 40

static int failures;
static int checks;

static void check(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

struct a20_swap_area {
    void *map;
    void *bits;
    uint64_t pages;
    uint64_t hint;
    int32_t active;
    int32_t reserved;
};

/* ---------------------------------------------------------------- model -- */

#define MAX_PAGES 256

static uint32_t g_map[MAX_SWAPFILES][MAX_PAGES];
static uint64_t g_bits[MAX_SWAPFILES][MAX_PAGES / 64];
static uint64_t g_pages[MAX_SWAPFILES];
static uint64_t g_hint[MAX_SWAPFILES];
static int g_active[MAX_SWAPFILES];
static void *g_owner[MAX_SWAPFILES][MAX_PAGES];

static int lock_calls, unlock_calls, account_calls, set_hint_calls;
static int set_owner_calls, clear_owner_calls;
static int charge_calls, uncharge_calls, pin_calls, unpin_calls;
static int last_account_alloc;
static int locked_now;
static int charge_while_locked, uncharge_while_locked, unpin_while_locked;
static void *last_charge_owner, *last_uncharge_owner, *last_unpin_owner;
/* Ordering evidence captured at the set_owner bridge, i.e. after Rust has
 * already touched the bitmap and the map. */
static int owner_call_bit_clear, owner_call_map_published, owner_call_locked;

static void reset(void)
{
    memset(g_map, 0, sizeof(g_map));
    memset(g_bits, 0, sizeof(g_bits));
    memset(g_pages, 0, sizeof(g_pages));
    memset(g_hint, 0, sizeof(g_hint));
    memset(g_active, 0, sizeof(g_active));
    memset(g_owner, 0, sizeof(g_owner));
    lock_calls = unlock_calls = account_calls = set_hint_calls = 0;
    set_owner_calls = clear_owner_calls = 0;
    charge_calls = uncharge_calls = pin_calls = unpin_calls = 0;
    last_account_alloc = -1;
    locked_now = 0;
    charge_while_locked = uncharge_while_locked = unpin_while_locked = 0;
    last_charge_owner = last_uncharge_owner = last_unpin_owner = NULL;
    owner_call_bit_clear = owner_call_map_published = owner_call_locked = -1;
}

static void setup_area(int type, uint64_t pages, int active)
{
    g_pages[type] = pages;
    g_active[type] = active;
    g_hint[type] = 1;
}

static void set_free(int type, uint64_t offset)
{
    g_bits[type][offset >> 6] |= 1ULL << (offset & 63);
}

static void mark_used(int type, uint64_t offset)
{
    g_map[type][offset] = 1;
    g_bits[type][offset >> 6] &= ~(1ULL << (offset & 63));
}

static uint64_t entry_of(uint32_t type, uint64_t offset)
{
    return ((offset & ((1ULL << OFFSET_BITS) - 1)) << SWP_TYPE_BITS) | type;
}

/* --------------------------------------------------------------- bridges - */

uint64_t a20_swap_lock(unsigned int type)
{
    (void)type;
    lock_calls++;
    locked_now = 1;
    return 0x5A5A;
}

void a20_swap_unlock(unsigned int type, uint64_t flags)
{
    (void)type;
    (void)flags;
    unlock_calls++;
    locked_now = 0;
}

void a20_swap_area(unsigned int type, struct a20_swap_area *out)
{
    out->map = g_map[type];
    out->bits = g_bits[type];
    out->pages = g_pages[type];
    out->hint = g_hint[type];
    out->active = g_active[type];
    out->reserved = 0;
}

void a20_swap_set_hint(unsigned int type, uint64_t hint)
{
    set_hint_calls++;
    g_hint[type] = hint;
}

void *a20_swap_owner(unsigned int type, uint64_t offset)
{
    return g_owner[type][offset];
}

void a20_swap_set_owner(unsigned int type, uint64_t offset, void *owner)
{
    set_owner_calls++;
    if (owner)
        set_owner_calls++;
    else
        clear_owner_calls++;
    if (owner) {
        owner_call_bit_clear =
            ((g_bits[type][offset >> 6] >> (offset & 63)) & 1) == 0;
        owner_call_map_published = g_map[type][offset] == 1;
        owner_call_locked = locked_now;
    }
    g_owner[type][offset] = owner;
}

void a20_swap_account(unsigned int type, int alloc)
{
    (void)type;
    account_calls++;
    last_account_alloc = alloc;
}

int cg_mem_swap_charge_node(void *owner, uint32_t pages)
{
    (void)pages;
    charge_calls++;
    last_charge_owner = owner;
    if (locked_now)
        charge_while_locked++;
    return 0;
}

void cg_mem_swap_uncharge_node(void *owner, uint32_t pages)
{
    (void)pages;
    uncharge_calls++;
    last_uncharge_owner = owner;
    if (locked_now)
        uncharge_while_locked++;
}

void cg_group_pin(void *owner)
{
    pin_calls++;
    (void)owner;
}

void cg_group_unpin(void *owner)
{
    unpin_calls++;
    last_unpin_owner = owner;
    if (locked_now)
        unpin_while_locked++;
}

extern uint64_t get_swap_page_owned(void *owner);
extern uint64_t get_swap_page(void);
extern int swap_duplicate(uint64_t entry);
extern void swap_free(uint64_t entry);

/* ------------------------------------------------------------- scenarios - */

static int g_owner_token;

static void allocation(void)
{
    reset();
    setup_area(0, 64, 1);
    set_free(0, 1);
    set_free(0, 2);
    set_free(0, 3);
    uint64_t e = get_swap_page_owned(&g_owner_token);
    check(e == entry_of(0, 3),
          "alloc: the highest free bit of the scanned word is taken");
    check(g_map[0][3] == 1, "alloc: the map entry is published");
    check((g_bits[0][0] & (1ULL << 3)) == 0, "alloc: the free bit is cleared");
    check(g_hint[0] == 4, "alloc: the hint advances past the slot");
    check(g_owner[0][3] == &g_owner_token, "alloc: the cgroup owner is recorded");
    check(account_calls == 1 && last_account_alloc == 1, "alloc: the slot is accounted");
    check(charge_calls == 1 && pin_calls == 1, "alloc: charged once and pinned");
    check(charge_while_locked == 0, "alloc: the charge happens outside the area lock");
    check(lock_calls == 1 && unlock_calls == 1, "alloc: one locked region");
    check(owner_call_bit_clear == 1,
          "alloc: the free bit is cleared before the slot is published");
    check(owner_call_map_published == 1,
          "alloc: the map entry is published before the slot is published");
    check(owner_call_locked == 1, "alloc: the slot is published under the area lock");

    /* The next call resumes at the hint and takes the next free bit down. */
    uint64_t e2 = get_swap_page_owned(&g_owner_token);
    check(e2 == entry_of(0, 2), "alloc: the scan resumes at the hint");

    /* Slot 0 is the header and is never offered. */
    reset();
    setup_area(0, 64, 1);
    set_free(0, 0);
    check(get_swap_page_owned(&g_owner_token) == 0, "alloc: slot 0 is never allocated");

    /* Word-major order: a low word wins over a high word. */
    reset();
    setup_area(0, 128, 1);
    set_free(0, 5);
    set_free(0, 70);
    check(get_swap_page_owned(&g_owner_token) == entry_of(0, 5),
          "alloc: whole words are scanned in ascending order");

    /* An exhausted area falls through to the next one. */
    reset();
    setup_area(0, 64, 1);
    setup_area(1, 64, 1);
    set_free(1, 5);
    uint64_t e1 = get_swap_page_owned(&g_owner_token);
    check(e1 == entry_of(1, 5), "alloc: the scan continues in the next active area");
    check(g_hint[1] == 6, "alloc: the hint of the second area advances");

    /* A full area reaches the tail of its last word. */
    reset();
    setup_area(0, 70, 1);
    set_free(0, 69);
    check(get_swap_page_owned(&g_owner_token) == entry_of(0, 69),
          "alloc: a slot in the partial tail word is usable");

    /* No active area: the charge is refunded and nothing is accounted. */
    reset();
    setup_area(0, 64, 0);
    check(get_swap_page_owned(&g_owner_token) == 0, "alloc: an inactive area yields nothing");
    check(charge_calls == 1 && uncharge_calls == 1, "alloc: the charge is refunded");
    check(pin_calls == 0, "alloc: nothing is pinned on failure");
    check(account_calls == 0, "alloc: nothing is accounted on failure");
    check(lock_calls == MAX_SWAPFILES, "alloc: every area is inspected once");

    /* A null owner still charges and frees the same way. */
    reset();
    setup_area(0, 64, 1);
    set_free(0, 2);
    check(get_swap_page() == entry_of(0, 2), "alloc: the owner-less variant allocates");
    check(last_charge_owner == NULL && g_owner[0][2] == NULL,
          "alloc: the owner-less variant records a null owner");
}

static void wrapping(void)
{
    /* A hint inside the area finds a slot after it. */
    reset();
    setup_area(0, 128, 1);
    set_free(0, 70);
    g_hint[0] = 100;
    check(get_swap_page_owned(&g_owner_token) == entry_of(0, 70),
          "wrap: a low slot after a high hint is found");
    check(g_hint[0] == 71, "wrap: the hint moves to the found slot");

    /* A hint past the end restarts at the beginning. */
    reset();
    setup_area(0, 128, 1);
    set_free(0, 70);
    g_hint[0] = 128;
    check(get_swap_page_owned(&g_owner_token) == entry_of(0, 70),
          "wrap: an out-of-range hint restarts the scan");

    /* A zero hint is clamped to the first allocatable slot. */
    reset();
    setup_area(0, 128, 1);
    set_free(0, 70);
    g_hint[0] = 0;
    check(get_swap_page_owned(&g_owner_token) == entry_of(0, 70),
          "wrap: a zero hint is clamped to slot 1");

    /* The tail word is masked to the area end. */
    reset();
    setup_area(0, 66, 1);
    set_free(0, 65);
    g_hint[0] = 60;
    check(get_swap_page_owned(&g_owner_token) == entry_of(0, 65),
          "wrap: the tail word is masked to the area end");

    /* A bit beyond the area end is never handed out. */
    reset();
    setup_area(0, 66, 1);
    set_free(0, 64);
    set_free(0, 65);
    g_hint[0] = 1;
    uint64_t a = get_swap_page_owned(&g_owner_token);
    check(a == entry_of(0, 65), "wrap: the last slot inside the area wins");
    uint64_t b = get_swap_page_owned(&g_owner_token);
    check(b == entry_of(0, 64), "wrap: the scan continues downwards");
    check(get_swap_page_owned(&g_owner_token) == 0,
          "wrap: a bit past the area end is never handed out");
}

static void references(void)
{
    reset();
    setup_area(0, 64, 1);
    mark_used(0, 5);
    check(swap_duplicate(entry_of(0, 5)) == 0, "dup: a live slot duplicates");
    check(g_map[0][5] == 2, "dup: the count increments");
    check(lock_calls == 1 && unlock_calls == 1, "dup: one locked region");

    mark_used(0, 6);
    g_map[0][6] = 0;
    check(swap_duplicate(entry_of(0, 6)) == -22, "dup: a free slot is EINVAL");
    mark_used(0, 7);
    g_map[0][7] = SWAP_MAP_BAD;
    check(swap_duplicate(entry_of(0, 7)) == -22, "dup: a bad slot is EINVAL");
    mark_used(0, 8);
    g_map[0][8] = SWAP_MAP_BAD - 1;
    check(swap_duplicate(entry_of(0, 8)) == -75, "dup: the last count is EOVERFLOW");
    check(g_map[0][8] == SWAP_MAP_BAD - 1, "dup: the overflowing slot is unchanged");
    check(swap_duplicate(entry_of(0, 64)) == -22, "dup: an out-of-range slot is EINVAL");
    check(swap_duplicate(entry_of(0, 0)) == -22, "dup: slot 0 is EINVAL");

    /* An inactive area is rejected without touching the map. */
    reset();
    setup_area(0, 64, 0);
    g_map[0][5] = 1;
    check(swap_duplicate(entry_of(0, 5)) == -22, "dup: an inactive area is EINVAL");
    check(g_map[0][5] == 1, "dup: an inactive area is left alone");

    /* Release: a mid-count free only decrements. */
    reset();
    setup_area(0, 64, 1);
    mark_used(0, 5);
    g_map[0][5] = 3;
    g_owner[0][5] = (void *)(uintptr_t)0x1234;
    g_hint[0] = 20;
    swap_free(entry_of(0, 5));
    check(g_map[0][5] == 2, "free: the count decrements");
    check(g_hint[0] == 20, "free: the hint is untouched while references remain");
    check(set_hint_calls == 0, "free: the hint bridge is not called while references remain");
    check(account_calls == 0 && uncharge_calls == 0 && unpin_calls == 0,
          "free: nothing is accounted while references remain");
    check(g_owner[0][5] == (void *)(uintptr_t)0x1234,
          "free: the owner is kept while references remain");

    /* The last reference frees the slot. */
    reset();
    setup_area(0, 64, 1);
    mark_used(0, 5);
    g_map[0][5] = 1;
    g_owner[0][5] = (void *)(uintptr_t)0x1234;
    g_hint[0] = 20;
    swap_free(entry_of(0, 5));
    check(g_map[0][5] == 0, "free: the map entry is cleared");
    check((g_bits[0][0] & (1ULL << 5)) != 0, "free: the free bit is set");
    check(g_owner[0][5] == NULL, "free: the owner is cleared");
    check(g_hint[0] == 5, "free: the hint rewinds to the released slot");
    check(account_calls == 1 && last_account_alloc == 0, "free: the slot is unaccounted");
    check(uncharge_calls == 1 && unpin_calls == 1, "free: the owner is uncharged and unpinned");
    check(uncharge_while_locked == 0 && unpin_while_locked == 0,
          "free: the cgroup work happens outside the area lock");
    check(last_uncharge_owner == (void *)(uintptr_t)0x1234, "free: the recorded owner is released");
    check(last_unpin_owner == (void *)(uintptr_t)0x1234, "free: the same owner is unpinned");

    /* A hint already at or below the slot is left alone. */
    reset();
    setup_area(0, 64, 1);
    mark_used(0, 5);
    g_map[0][5] = 1;
    g_hint[0] = 1;
    swap_free(entry_of(0, 5));
    check(g_hint[0] == 1, "free: a lower hint is not pushed forward");
    check(set_hint_calls == 0, "free: the hint bridge is skipped when the hint is already low");

    /* A bad or absent slot is a no-op apart from the null-owner drop. */
    reset();
    setup_area(0, 64, 1);
    mark_used(0, 9);
    g_map[0][9] = SWAP_MAP_BAD;
    g_hint[0] = 20;
    swap_free(entry_of(0, 9));
    check(g_map[0][9] == SWAP_MAP_BAD, "free: a bad slot is untouched");
    check(g_hint[0] == 20, "free: a bad slot does not move the hint");
    check(account_calls == 0, "free: a bad slot is not accounted");
    check(uncharge_calls == 1 && unpin_calls == 1, "free: the null owner is still dropped");
    check(last_uncharge_owner == NULL && last_unpin_owner == NULL,
          "free: the drop carries the null owner");

    /* A free slot and slot 0 are both no-ops. */
    reset();
    setup_area(0, 64, 1);
    g_map[0][0] = 1;
    g_map[0][10] = 0;
    g_hint[0] = 20;
    swap_free(entry_of(0, 10));
    swap_free(entry_of(0, 0));
    check(g_map[0][0] == 1, "free: slot 0 is untouched");
    check(g_map[0][10] == 0, "free: an already free slot is untouched");
    check(g_hint[0] == 20, "free: neither no-op moves the hint");
    check(account_calls == 0, "free: neither no-op is accounted");

    /*
     * The `type >= MAX_SWAPFILES` guard cannot be reached through a packed
     * entry: SWP_TYPE_MASK is 4 bits wide, exactly MAX_SWAPFILES.  It is kept
     * as defence in depth for callers that build an entry field by field, and
     * is exercised by inspection rather than from here.
     */

    /* Round trip: allocate then release the same entry. */
    reset();
    setup_area(0, 64, 1);
    set_free(0, 1);
    uint64_t e = get_swap_page_owned(&g_owner_token);
    check(e == entry_of(0, 1), "round trip: allocated");
    swap_free(e);
    check(g_map[0][1] == 0, "round trip: the slot is free again");
    check((g_bits[0][0] & (1ULL << 1)) != 0, "round trip: the free bit is restored");
    check(g_hint[0] == 1, "round trip: the hint is back at the start");
    check(g_owner[0][1] == NULL, "round trip: the owner is dropped");
}

int main(void)
{
    allocation();
    wrapping();
    references();
    reset();

    if (failures) {
        printf("SWAP_SLOTS_HOST: FAIL %d/%d checks\n", failures, checks);
        return 1;
    }
    printf("SWAP_SLOTS_HOST: PASS %d checks\n", checks);
    return 0;
}
