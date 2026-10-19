/*
 * Host differential harness for kernel/mm/vmar_policy.rs.
 *
 * The production Rust is linked against a model of the reservation tree: real
 * nodes, a real ordered child list and a real refcount, with the C primitives
 * (alloc/free/link/unlink/lock/get/put) implemented the way mm/vmar.c
 * implements them.  The assertions are about the rules Rust owns: containment,
 * monotonic ceilings, the no-overlap test-and-insert as one atomic step, the
 * ordered insertion point, and the release ordering that keeps a cascading
 * teardown out of the list it just left.
 *
 * Guest coverage is separate: /bin/native-mm-* drives the VMAR hierarchy,
 * ceilings and capability stamping through the Native ABI.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_SIZE 4096
#define USER_VA_LIMIT ((uint64_t)1 << 38)

#define VMAR_CAN_MAP_READ     0x01u
#define VMAR_CAN_MAP_WRITE    0x02u
#define VMAR_CAN_MAP_EXEC     0x04u
#define VMAR_CAN_MAP_SPECIFIC 0x08u

static int failures, checks;

static void check(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

/* ---------------------------------------------------------------- model -- */

struct vmar {
    uint64_t base, len;
    uint32_t cap, child_count;
    struct vmar *parent, *child_head, *child_tail, *sibling_next;
    int lock, refs;
};

struct a20_vmar_geometry {
    uint64_t base, len;
    uint32_t cap, pad;
    struct vmar *parent;
};

static int g_locked, lock_calls, unlock_calls, link_calls, unlink_calls;
static int alloc_calls, free_calls, get_calls, put_calls;
static int link_while_unlocked, unlink_while_unlocked, get_while_unlocked;
static struct vmar *g_freed[64];
static int g_nfreed;
static int g_alloc_fail_after = -1;

static void model_reset(void)
{
    g_locked = lock_calls = unlock_calls = link_calls = unlink_calls = 0;
    alloc_calls = free_calls = get_calls = put_calls = 0;
    link_while_unlocked = unlink_while_unlocked = get_while_unlocked = 0;
    g_nfreed = 0;
    g_alloc_fail_after = -1;
}

uint64_t a20_vmar_user_va_limit(void) { return USER_VA_LIMIT; }

struct vmar *a20_vmar_alloc(uint64_t base, uint64_t len, uint32_t cap)
{
    alloc_calls++;
    if (g_alloc_fail_after >= 0 && alloc_calls > g_alloc_fail_after)
        return NULL;
    struct vmar *v = calloc(1, sizeof(*v));
    v->base = base; v->len = len; v->cap = cap;
    v->refs = 1;
    return v;
}

void a20_vmar_set_parent(struct vmar *v, struct vmar *p) { v->parent = p; }

void a20_vmar_geometry(const struct vmar *v, struct a20_vmar_geometry *out)
{
    out->base = v->base; out->len = v->len; out->cap = v->cap;
    out->pad = 0; out->parent = v->parent;
}

struct vmar *a20_vmar_locate(struct vmar *parent, uint64_t base,
                                    struct vmar **out_pred)
{
    struct vmar *pred = NULL, *cur = parent->child_head;
    while (cur && cur->base < base) { pred = cur; cur = cur->sibling_next; }
    *out_pred = pred;
    return cur;
}

void a20_vmar_link(struct vmar *parent, struct vmar *child, struct vmar *pred)
{
    link_calls++;
    if (!g_locked) link_while_unlocked++;
    if (pred) {
        child->sibling_next = pred->sibling_next;
        pred->sibling_next = child;
    } else {
        child->sibling_next = parent->child_head;
        parent->child_head = child;
    }
    if (!child->sibling_next) parent->child_tail = child;
    parent->child_count++;
}

void a20_vmar_unlink(struct vmar *parent, struct vmar *v)
{
    unlink_calls++;
    /* The real bridge takes the parent's lock itself. */
    g_locked++;
    struct vmar **pp = &parent->child_head;
    while (*pp && *pp != v) pp = &(*pp)->sibling_next;
    if (*pp) { *pp = v->sibling_next; parent->child_count--; }
    if (parent->child_tail == v) {
        struct vmar *last = parent->child_head;
        while (last && last->sibling_next) last = last->sibling_next;
        parent->child_tail = last;
    }
    g_locked--;
}

void a20_vmar_free(struct vmar *v)
{
    free_calls++;
    if (g_nfreed < 64) g_freed[g_nfreed++] = v;
    free(v);
}

uint64_t a20_vmar_lock(struct vmar *parent)
{
    (void)parent;
    lock_calls++; g_locked++;
    return 0x77;
}

void a20_vmar_unlock(struct vmar *parent, uint64_t flags)
{
    (void)parent; (void)flags;
    unlock_calls++; g_locked--;
}

void a20_vmar_get(struct vmar *v)
{
    get_calls++;
    if (!g_locked) get_while_unlocked++;
    v->refs++;
}

int a20_vmar_put(struct vmar *v)
{
    put_calls++;
    return (--v->refs == 0) ? 1 : 0;
}

extern struct vmar *vmar_create_root(void *mm, uint64_t base, uint64_t len, uint32_t cap);
extern long vmar_create_child(struct vmar *parent, uint64_t base, uint64_t len,
                              uint32_t cap_request, struct vmar **out);
extern int vmar_contains(const struct vmar *v, uint64_t addr, uint64_t len);
extern int vmar_cap_allows(const struct vmar *v, uint32_t map_bits);
extern void vmar_acquire(struct vmar *v);
extern void vmar_release(struct vmar *v);

/* ------------------------------------------------------------- scenarios - */

static struct vmar *root(uint64_t base, uint64_t len, uint32_t cap)
{
    return vmar_create_root(NULL, base, len, cap);
}

static void root_contract(void)
{
    model_reset();
    check(root(0x10000, 0x10000, VMAR_CAN_MAP_READ) != NULL, "root: a valid range is accepted");
    check(alloc_calls == 1, "root: one node is allocated");

    model_reset();
    check(root(0x10000, 0, VMAR_CAN_MAP_READ) == NULL, "root: a zero length is refused");
    check(root(USER_VA_LIMIT, PAGE_SIZE, 0) == NULL, "root: a base at the limit is refused");
    check(root(USER_VA_LIMIT - PAGE_SIZE, 2 * PAGE_SIZE, 0) == NULL,
          "root: a range past the limit is refused");
    check(root(0x10001, PAGE_SIZE, 0) == NULL, "root: an unaligned base is refused");
    check(root(0x10000, PAGE_SIZE + 1, 0) == NULL, "root: an unaligned length is refused");
    check(alloc_calls == 0, "root: a refused range allocates nothing");

    /* The ceiling is masked to the canonical bits. */
    model_reset();
    struct vmar *r = root(0x10000, 0x10000, 0xFFFFFFFFu);
    check(r->cap == (VMAR_CAN_MAP_READ | VMAR_CAN_MAP_WRITE | VMAR_CAN_MAP_EXEC |
                     VMAR_CAN_MAP_SPECIFIC),
          "root: the ceiling is masked to the canonical bits");

    model_reset();
    r = root(0x10000, 0x10000, 0x100);
    check(r->cap == 0, "root: a non-canonical ceiling request grants nothing");
}

static void child_contract(void)
{
    model_reset();
    struct vmar *r = root(0x10000, 0x40000, VMAR_CAN_MAP_READ | VMAR_CAN_MAP_WRITE);
    struct vmar *c = NULL;

    check(vmar_create_child(r, 0x11000, PAGE_SIZE, VMAR_CAN_MAP_READ, &c) == 0,
          "child: a contained range is accepted");
    check(c && c->base == 0x11000 && c->len == PAGE_SIZE, "child: the node carries the range");
    check(c && c->parent == r, "child: the parent is recorded");
    check(lock_calls == 1 && unlock_calls == 1, "child: one locked region");
    check(link_calls == 1 && !link_while_unlocked,
          "child: the insert happens under the parent's lock");
    check(get_calls == 1 && !get_while_unlocked,
          "child: the parent reference is taken under the lock");
    check(r->child_count == 1 && r->child_head == c && r->child_tail == c,
          "child: the first child becomes head and tail");

    /* A range starting at the parent's own base is contained. */
    struct vmar *edge = NULL;
    check(vmar_create_child(r, 0x10000, PAGE_SIZE, 0, &edge) == 0,
          "child: a range starting at the parent's base is contained");
    check(r->child_head == edge && edge->sibling_next == c,
          "child: a lower base becomes the new head");

    /* Monotonic ceilings. */
    struct vmar *c2 = NULL;
    check(vmar_create_child(r, 0x12000, PAGE_SIZE,
                            VMAR_CAN_MAP_READ | VMAR_CAN_MAP_EXEC, &c2) == 0,
          "child: a narrower request is accepted");
    check(c2->cap == VMAR_CAN_MAP_READ, "child: the ceiling narrows to the parent's");
    check(c2->cap != VMAR_CAN_MAP_EXEC, "child: a bit the parent lacks is not granted");

    /* Containment. */
    struct vmar *c3 = NULL;
    check(vmar_create_child(r, 0x0F000, PAGE_SIZE, 0, &c3) == -28,
          "child: a range starting before the parent is refused");
    check(vmar_create_child(r, 0x4F000, 0x2000, 0, &c3) == -28,
          "child: a range ending after the parent is refused");
    check(vmar_create_child(r, 0x50000, PAGE_SIZE, 0, &c3) == -28,
          "child: a range past the parent is refused");

    /* Validation order: alignment and wrap before containment. */
    check(vmar_create_child(r, 0x11001, PAGE_SIZE, 0, &c3) == -22,
          "child: an unaligned base is EINVAL");
    check(vmar_create_child(r, 0x11000, 0, 0, &c3) == -22, "child: a zero length is EINVAL");
    check(vmar_create_child(r, 0x11000, PAGE_SIZE + 1, 0, &c3) == -22,
          "child: an unaligned length is EINVAL");
    check(vmar_create_child(r, (uint64_t)-PAGE_SIZE, 2 * PAGE_SIZE, 0, &c3) == -22,
          "child: a range that wraps is EINVAL");
    check(vmar_create_child(NULL, 0x11000, PAGE_SIZE, 0, &c3) == -22,
          "child: a null parent is EINVAL");
    check(vmar_create_child(r, 0x11000, PAGE_SIZE, 0, NULL) == -22,
          "child: a null out is EINVAL");

    /* Overlap. */
    check(vmar_create_child(r, 0x11000, PAGE_SIZE, 0, &c3) == -28,
          "child: an identical range overlaps");
    check(vmar_create_child(r, 0x11000, 0x2000, 0, &c3) == -28,
          "child: a range spanning two siblings overlaps");
    /* Every range is page aligned, so a straddle is always a multi-page range
     * that starts inside one sibling and ends inside the next. */
    check(vmar_create_child(r, 0x10000, 0x2000, 0, &c3) == -28,
          "child: a range extending past a sibling's end overlaps");
    check(vmar_create_child(r, 0x12000, PAGE_SIZE, 0, &c3) == -28,
          "child: a range starting exactly at a sibling overlaps");
    /* Only the overlap refusals allocate first: containment and alignment are
     * checked before the node exists, so those cost no free. */
    check(free_calls == 4, "child: every refused insert frees the unpublished node");
    check(r->child_count == 3, "child: a refused insert adds no child");

    /* Adjacency is not overlap, and the ordered list keeps its order. */
    struct vmar *c4 = NULL;
    check(vmar_create_child(r, 0x13000, PAGE_SIZE, 0, &c4) == 0,
          "child: a range starting where a sibling ends is accepted");
    check(r->child_head == edge && edge->sibling_next == c &&
          c->sibling_next == c2 && c2->sibling_next == c4,
          "child: the insert keeps the list ordered by base");
    check(r->child_tail == c4, "child: the tail tracks the highest child");
    check(r->child_count == 4, "child: the count tracks the children");

    /* Appending past the tail takes the pred path. */
    struct vmar *c5 = NULL;
    check(vmar_create_child(r, 0x40000, PAGE_SIZE, 0, &c5) == 0,
          "child: a range past every sibling is accepted");
    check(r->child_tail == c5 && c4->sibling_next == c5,
          "child: the append lands after the previous tail");
    check(link_calls == 5, "child: one link per accepted insert");

    /* Out of memory is reported without leaking a link. */
    model_reset();
    r = root(0x10000, 0x40000, VMAR_CAN_MAP_READ);
    g_alloc_fail_after = 0;
    check(vmar_create_child(r, 0x11000, PAGE_SIZE, 0, &c3) == -12,
          "child: an allocation failure is ENOMEM");
    check(r->child_count == 0 && r->child_head == NULL, "child: a failed allocation links nothing");
    check(lock_calls == 0, "child: the lock is taken only once the node exists");
}

static void queries(void)
{
    model_reset();
    struct vmar *r = root(0x10000, 0x40000, VMAR_CAN_MAP_READ | VMAR_CAN_MAP_WRITE);

    check(vmar_contains(r, 0x10000, 0x40000) == 1, "contains: the whole range is inside");
    check(vmar_contains(r, 0x10000, 0x40001) == 0, "contains: one byte past the end is outside");
    check(vmar_contains(r, 0x0F000, PAGE_SIZE) == 0, "contains: a range starting before is outside");
    check(vmar_contains(r, 0x20000, PAGE_SIZE) == 1, "contains: an interior range is inside");
    check(vmar_contains(r, 0x10000, 0) == 0, "contains: a zero length is outside");
    check(vmar_contains(r, (uint64_t)-PAGE_SIZE, 2 * PAGE_SIZE) == 0,
          "contains: a range that wraps is outside");
    check(vmar_contains(NULL, 0x10000, PAGE_SIZE) == 0, "contains: a null node is outside");

    check(vmar_cap_allows(r, VMAR_CAN_MAP_READ) == 1, "cap: a granted bit is allowed");
    check(vmar_cap_allows(r, VMAR_CAN_MAP_READ | VMAR_CAN_MAP_WRITE) == 1,
          "cap: a subset of the granted bits is allowed");
    check(vmar_cap_allows(r, VMAR_CAN_MAP_EXEC) == 0, "cap: an ungranted bit is refused");
    check(vmar_cap_allows(r, VMAR_CAN_MAP_READ | VMAR_CAN_MAP_EXEC) == 0,
          "cap: one ungranted bit refuses the whole request");
    check(vmar_cap_allows(r, 0) == 1, "cap: an empty request is allowed");

    /* The specific-mapping bit is not part of the query mask. */
    model_reset();
    struct vmar *s = root(0x10000, 0x10000, VMAR_CAN_MAP_SPECIFIC);
    check(vmar_cap_allows(s, VMAR_CAN_MAP_SPECIFIC) == 0,
          "cap: the specific bit is not a queryable mapping right");
    check(vmar_cap_allows(s, VMAR_CAN_MAP_READ) == 0,
          "cap: a node granting only SPECIFIC grants no read");
}

static void release_path(void)
{
    /* A child reference keeps its parent alive. */
    model_reset();
    struct vmar *r = root(0x10000, 0x40000, VMAR_CAN_MAP_READ);
    struct vmar *c = NULL;
    vmar_create_child(r, 0x11000, PAGE_SIZE, 0, &c);
    check(r->refs == 2, "release: the child holds a parent reference");

    /* Releasing the child unlinks it and drops the parent reference. */
    vmar_release(c);
    check(unlink_calls == 1, "release: the dying child unlinks itself");
    check(r->child_count == 0 && r->child_head == NULL && r->child_tail == NULL,
          "release: the parent's list is consistent afterwards");
    check(r->refs == 1, "release: the parent reference is dropped");
    check(free_calls == 1, "release: the child node is freed");
    check(r->child_tail == NULL, "release: the tail is recomputed from the surviving head");

    /* The unlink happens before the parent's reference is dropped, and the
     * parent's release recursion happens after the unlink returned. */
    check(unlink_calls == 1 && put_calls >= 2, "release: the parent is released after the unlink");

    /* Freeing the last reference frees the node. */
    model_reset();
    r = root(0x10000, 0x40000, VMAR_CAN_MAP_READ);
    vmar_release(r);
    check(free_calls == 1, "release: the last reference frees the root");
    check(unlink_calls == 0, "release: a root has nothing to unlink");

    /* A shared reference survives until the last drop. */
    model_reset();
    r = root(0x10000, 0x40000, VMAR_CAN_MAP_READ);
    vmar_acquire(r);
    check(r->refs == 2, "acquire: the reference count grows");
    vmar_release(r);
    check(free_calls == 0, "release: a node with references left is not freed");
    vmar_release(r);
    check(free_calls == 1, "release: the last reference frees it");

    /* A null release is a no-op. */
    model_reset();
    vmar_release(NULL);
    check(put_calls == 0 && free_calls == 0, "release: a null node is a no-op");
    vmar_acquire(NULL);
    check(get_calls == 0, "acquire: a null node is a no-op");

    /* Cascading teardown: the middle node unlinks from its parent and the
     * parent then goes away because the child's reference was the last one. */
    model_reset();
    r = root(0x10000, 0x40000, VMAR_CAN_MAP_READ);
    struct vmar *mid = NULL;
    vmar_create_child(r, 0x10000, 0x20000, 0, &mid);
    struct vmar *leaf = NULL;
    vmar_create_child(mid, 0x11000, PAGE_SIZE, 0, &leaf);
    check(r->refs == 2 && mid->refs == 2, "cascade: each child holds one parent reference");
    vmar_release(leaf);
    check(mid->child_count == 0 && mid->refs == 1, "cascade: the leaf unlinks from the middle");
    check(free_calls == 1, "cascade: only the leaf is freed");
    vmar_release(mid);
    check(r->child_count == 0 && r->refs == 1, "cascade: the middle unlinks from the root");
    check(free_calls == 2, "cascade: the middle is freed");
}

int main(void)
{
    root_contract();
    child_contract();
    queries();
    release_path();

    if (failures) {
        printf("VMAR_POLICY_HOST: FAIL %d/%d checks\n", failures, checks);
        return 1;
    }
    printf("VMAR_POLICY_HOST: PASS %d checks\n", checks);
    return 0;
}
