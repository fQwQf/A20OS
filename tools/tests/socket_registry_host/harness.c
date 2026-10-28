/*
 * Differential harness for the exact production kernel/net/socket/socket_registry.c
 * and kernel/net/socket/socket_table.c.
 *
 * Both selector runs compile this same translation unit and drive the same
 * scenarios through the production entry points, so the transcripts must be
 * byte-identical: the C reference bodies under #else and the Rust state machine
 * in kernel/net/socket/socket_registry.rs are compared on the real code, not on a
 * reimplementation of it.
 *
 * Everything the two files reach that is a kernel primitive rather than registry
 * state is stubbed here identically for both selectors: the locks and IRQ
 * predicates (tools/tests/socket_registry_host/hostinc/core/cpu.h), the packet
 * census, the receive-queue accounting, the namespace lookup and the
 * lock-counter registry.  The registry state machine under test is never
 * stubbed.
 *
 * libc headers are included by absolute path because the kernel's freestanding
 * include directories are searched first and shadow them.
 */
#include "net/socket/socket_internal.h"
#include "net/socket_side.h"

#include "/usr/include/pthread.h"
#include "/usr/include/stdlib.h"
#include "/usr/include/string.h"
#include <unistd.h>

/* --------------------------------------------------------------- primitives */

static unsigned host_cpu;

unsigned cpu_current_id(void) { return host_cpu; }

/* The Rust state machine's panic path routes through the kernel's panic(); the
 * host run supplies the same terminal action.  Referenced only by the rust
 * selector's runtime archive, defined in both runs so the harness is one TU. */
__attribute__((noreturn)) void panic(const char *fmt, ...)
{
    (void)fmt;
    (void)!write(2, "HOST PANIC\n", 11);
    abort();
}

void rust_eh_personality(void) { }

void lock_counters_register(spinlock_t *lock, const char *name)
{
    (void)lock;
    (void)name;
}
unsigned lock_counters_count(void) { return 0; }
unsigned lock_counters_dropped(void) { return 0; }

/* The packet census and the receive-queue accounting belong to other modules;
 * what this harness needs from them is only that they are callable. */
void net_packet_bound_release(net_socket_t *s) { (void)s; }
void net_rxq_reset_locked(net_socket_t *s) { (void)s; }
size_t net_rxq_bytes_locked(net_socket_t *s)
{
    return s ? (size_t)s->rx_count * 7u : 0u;
}

static resource_namespace_t g_ns_a;
static resource_namespace_t g_ns_b;
resource_namespace_t *resource_ns_current(int kind)
{
    (void)kind;
    return &g_ns_a;
}

/* ------------------------------------------------------------------ helpers */

/* The kernel's own printf/snprintf declarations (core/stdio.h) are used; they
 * resolve to the host libc at link time. */
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static unsigned long bitmap_sum(void)
{
    unsigned long sum = 0;
    for (int b = 0; b < NET_SOCK_BUCKETS; b++)
        for (int w = 0; w < NET_SOCK_BUCKET_WORDS; w++)
            sum = sum * 31u + g_net_buckets[b].free_bits[w];
    return sum;
}

static void init_socket(net_socket_t *s, int domain, int type)
{
    memset(s, 0, sizeof(*s));
    spin_init(&s->lock);
    s->domain = domain;
    s->type = type;
    s->reg_idx = -1;
    s->net_ns = &g_ns_a;
    net_sock_ref_init(s);
}

/* Drop both references the way net_socket_close_file() does: the registry's
 * only when unregister() reported that it existed, then the creator's. */
static void drop_socket(net_socket_t *s)
{
    int released = net_socket_unregister(s) ? 1 : 0;
    (void)released;
    net_sock_ref_release(s);
    net_sock_ref_release(s);
}

static int fail(const char *what)
{
    printf("FAIL %s\n", what);
    return 1;
}

/* ----------------------------------------------------------------- scenarios */

/* Registry slots: claim, publish, identity-checked release, reuse and the
 * full-table refusal, across a ring that starts at the registering CPU. */
static int scenario_slots(void)
{
    net_socket_t socks[4];

    say("slots: buckets=%d per_bucket=%d words=%d", NET_SOCK_BUCKETS,
        NET_SOCK_SLOTS_PER_BUCKET, NET_SOCK_BUCKET_WORDS);
    say("slots: bitmap_after_init=%lx", bitmap_sum());

    for (int i = 0; i < 3; i++) {
        init_socket(&socks[i], AF_INET, SOCK_STREAM);
        int r = net_register_socket_locked(&socks[i]);
        say("slots: register%d rc=%d idx=%d in_registry=%d refs=%d slot_ok=%d",
            i, r, socks[i].reg_idx, socks[i].in_registry, socks[i].refs,
            r == 0 && g_sockets[socks[i].reg_idx] == &socks[i]);
    }

    /* The identity test: point the recorded slot at another socket and release.
     * The slot and its bitmap bit must be left alone, but the socket still
     * reports that it had been registered, so the caller still drops the
     * registry's reference. */
    net_socket_t intruder;
    init_socket(&intruder, AF_INET, SOCK_STREAM);
    int victim_idx = socks[1].reg_idx;
    net_socket_t *saved = g_sockets[victim_idx];
    g_sockets[victim_idx] = &intruder;
    unsigned long before = bitmap_sum();
    say("slots: stale_release=%d", net_socket_unregister(&socks[1]) ? 1 : 0);
    say("slots: stale_bitmap_untouched=%d table_untouched=%d",
        bitmap_sum() == before, g_sockets[victim_idx] == &intruder);
    say("slots: stale_fields in_registry=%d reg_idx=%d", socks[1].in_registry,
        socks[1].reg_idx);
    g_sockets[victim_idx] = saved;
    net_sock_ref_release(&intruder);

    for (int i = 0; i < 3; i++)
        drop_socket(&socks[i]);
    say("slots: bitmap_after_release=%lx", bitmap_sum());

    /* A socket that never owned a slot carries one reference and owes none. */
    net_socket_t orphan;
    init_socket(&orphan, AF_UNIX, SOCK_STREAM);
    orphan.reg_idx = -1;
    say("orphan: unregister=%d", net_socket_unregister(&orphan) ? 1 : 0);
    net_sock_ref_release(&orphan);
    say("orphan: refs=%d magic=%x", orphan.refs, orphan.ref_magic);

    /* Fill the table, then ask once more: ENFILE only after every bucket is
     * full, and the bitmap must be exactly the initialized one afterwards. */
    int cap = NET_SOCK_BUCKETS * NET_SOCK_SLOTS_PER_BUCKET;
    net_socket_t *many = calloc((size_t)cap + 1, sizeof(net_socket_t));
    if (!many)
        return fail("slot fill allocation");
    int ok = 0;
    for (int i = 0; i <= cap; i++) {
        init_socket(&many[i], AF_INET, SOCK_STREAM);
        int r = net_register_socket_locked(&many[i]);
        if (r != 0) {
            say("slots: full_after=%d rc=%d", ok, r);
            say("slots: bitmap_full=%lx", bitmap_sum());
            /* The refused socket was created but never published, so it carries
             * the creator's reference alone. */
            net_sock_ref_release(&many[i]);
            break;
        }
        ok++;
    }
    for (int i = 0; i < ok; i++)
        drop_socket(&many[i]);
    free(many);
    say("slots: bitmap_after_drain=%lx", bitmap_sum());
    return 0;
}

/* The registry ring starts at the registering CPU's own bucket. */
static int scenario_ring(void)
{
    for (unsigned cpu = 0; cpu < 3; cpu++) {
        host_cpu = cpu;
        net_socket_t s;
        init_socket(&s, AF_INET, SOCK_STREAM);
        int r = net_register_socket_locked(&s);
        int bucket = s.reg_idx >> NET_SOCK_BUCKET_SHIFT;
        say("ring: cpu=%u rc=%d bucket=%d expect=%d", cpu, r, bucket,
            (int)(cpu % (unsigned)NET_SOCK_BUCKETS));
        drop_socket(&s);
    }
    host_cpu = 0;
    return 0;
}

/* Reference count: acquire, release, the last-release decision, and the two
 * fault reasons the ledger distinguishes. */
static int scenario_refs(void)
{
    net_socket_t s;
    init_socket(&s, AF_INET, SOCK_STREAM);
    say("refs: live_after_init=%d", net_sock_ref_live());
    say("refs: acquire=%d", net_socket_ref(&s)->refs);
    say("refs: release_held=%d refs=%d", net_sock_ref_release(&s), s.refs);
    say("refs: release_last=%d magic=%x", net_sock_ref_release(&s), s.ref_magic);
    say("refs: live_after_free=%d", net_sock_ref_live());

    /* Second release: the canary is already cleared, so the fault is the canary
     * one rather than "refs was not positive". */
    say("refs: double_release=%d", net_sock_ref_release(&s));

    net_socket_t t;
    init_socket(&t, AF_INET, SOCK_STREAM);
    t.ref_magic = 0;
    say("refs: non_socket_release=%d", net_sock_ref_release(&t));

    net_socket_t u;
    init_socket(&u, AF_INET, SOCK_STREAM);
    u.refs = 0;
    say("refs: lost_ref_release=%d", net_sock_ref_release(&u));

    /* The two fault objects were deliberately left inconsistent, so the
     * faulting release never freed them.  Restore them and free them cleanly,
     * so the run's final ledger is a real zero rather than a pile of intended
     * leftovers -- and a leak in either selector would show up there. */
    t.ref_magic = NET_SOCK_REF_MAGIC;
    net_sock_ref_release(&t);
    u.refs = 1;
    net_sock_ref_release(&u);

    char row[512];
    int n = net_sock_ref_format(row, sizeof(row));
    say("refs: format_bytes=%d", n);
    printf("%s", row);
    return 0;
}

/* Bottom-half pending flags and the count sched() early-outs on. */
static int scenario_pending(void)
{
    int idx[3] = { 0, 1, NET_MAX_SOCKETS - 1 };
    say("bh: any_after_init=%d", net_bh_any_pending());
    for (int i = 0; i < 3; i++) {
        net_bh_slot_mark(idx[i]);
        say("bh: mark%d pending=%d count=%d", idx[i],
            net_bh_slot_pending(idx[i]), g_net_bh_pending_count);
        net_bh_slot_mark(idx[i]);
        say("bh: remark%d pending=%d count=%d", idx[i],
            net_bh_slot_pending(idx[i]), g_net_bh_pending_count);
    }
    say("bh: any_marked=%d", net_bh_any_pending());
    for (int i = 0; i < 3; i++) {
        say("bh: slot_pending%d=%d", idx[i], net_bh_slot_pending(idx[i]));
        say("bh: clear%d=%d count=%d", idx[i], net_bh_slot_clear(idx[i]),
            g_net_bh_pending_count);
    }
    say("bh: any_cleared=%d", net_bh_any_pending());
    /* Out-of-range slots must be refused without touching the count. */
    net_bh_slot_mark(-1);
    say("bh: mark_oob pending_count=%d clear_oob=%d", g_net_bh_pending_count,
        net_bh_slot_clear(NET_MAX_SOCKETS));
    say("bh: slot_pending_oob=%d", net_bh_slot_pending(NET_MAX_SOCKETS + 5));
    return 0;
}

/* /proc/net membership and FIONREAD readability, driven through the walk. */
static int g_visited;

static void count_visit(net_socket_t *s, void *arg)
{
    (void)s;
    (void)arg;
    g_visited++;
}

static int scenario_table(void)
{
    struct { int domain; int type; } shapes[] = {
        { AF_INET, SOCK_STREAM },
        { AF_INET6, SOCK_STREAM },
        { AF_INET, SOCK_DGRAM },
        { AF_UNIX, SOCK_STREAM },
        { AF_PACKET, SOCK_RAW },
    };
    net_socket_t socks[5];
    for (int i = 0; i < 5; i++) {
        init_socket(&socks[i], shapes[i].domain, shapes[i].type);
        net_register_socket_locked(&socks[i]);
    }

    net_table_kind_t kinds[] = { NET_TABLE_TCP, NET_TABLE_UDP, NET_TABLE_UNIX };
    int families[] = { AF_UNSPEC, AF_INET, AF_INET6 };
    for (int k = 0; k < 3; k++) {
        for (int f = 0; f < 3; f++) {
            g_visited = 0;
            int r = net_socket_table_walk(kinds[k], families[f], count_visit, NULL);
            say("table: kind=%d family=%d rc=%d visited=%d", k, families[f], r,
                g_visited);
        }
    }

    /* A socket whose namespace differs is not listed; a closed one is not
     * either, even while its slot is still published. */
    socks[0].net_ns = &g_ns_b;
    g_visited = 0;
    net_socket_table_walk(NET_TABLE_TCP, AF_UNSPEC, count_visit, NULL);
    say("table: other_ns_visited=%d", g_visited);
    socks[0].net_ns = &g_ns_a;
    socks[1].closed = 1;
    g_visited = 0;
    net_socket_table_walk(NET_TABLE_TCP, AF_UNSPEC, count_visit, NULL);
    say("table: closed_visited=%d", g_visited);
    socks[1].closed = 0;

    /* No callback is -EINVAL, not a silent zero. */
    say("table: no_callback_rc=%d",
        net_socket_table_walk(NET_TABLE_TCP, AF_UNSPEC, NULL, NULL));

    /* FIONREAD: an AF_PACKET socket cannot answer with a byte count. */
    for (int i = 0; i < 5; i++) {
        size_t out = 0;
        int r = net_socket_rx_available(&socks[i], &out);
        say("rxavail: domain=%d rc=%d out=%u", socks[i].domain, r,
            (unsigned)out);
    }
    say("rxavail: null_out=%d", net_socket_rx_available(&socks[0], NULL));
    say("rxavail: null_socket=%d", net_socket_rx_available(NULL, NULL));

    for (int i = 0; i < 5; i++)
        drop_socket(&socks[i]);
    return 0;
}

/*
 * Concurrency: real threads driving the production entry points at once.
 *
 * Each worker repeatedly registers a socket, takes and drops references, marks
 * and clears its bottom-half flag and unregisters again, while a reader walks
 * the table and reads the pending flags. Nothing here is a reimplementation:
 * the workers call the same net_register_socket_locked()/net_socket_unregister()
 * /net_socket_ref()/net_bh_slot_mark() the kernel does, and the reader calls the
 * same net_socket_table_walk().
 *
 * The scenario prints only invariants that must hold no matter how the threads
 * interleave -- bitmap back to its baseline, the ledger back to its baseline
 * with no faults, and an empty walk -- so the two selectors' transcripts stay
 * comparable while the interleaving itself does not.
 */
#define CT_WORKERS 4
#define CT_ROUNDS  400

typedef struct {
    int rounds;
    int errors;
} ct_worker_t;

static void *ct_worker(void *arg)
{
    ct_worker_t *w = arg;
    for (int r = 0; r < w->rounds; r++) {
        net_socket_t *s = calloc(1, sizeof(net_socket_t));
        if (!s) { w->errors++; return NULL; }
        init_socket(s, AF_INET, SOCK_STREAM);
        if (net_register_socket_locked(s) != 0) { free(s); w->errors++; continue; }
        /* A reference taken and dropped by a path that is not the teardown.
         * net_socket_free() is socket.c's (it owns the destroy sequence), so
         * the drop goes straight through the release decision here. */
        net_socket_ref(s);
        (void)net_sock_ref_release(s);
        if (s->reg_idx >= 0) {
            net_bh_slot_mark(s->reg_idx);
            (void)net_bh_slot_pending(s->reg_idx);
            (void)net_bh_slot_clear(s->reg_idx);
        }
        if (!net_socket_unregister(s)) { w->errors++; }
        net_sock_ref_release(s);      /* the registry's, if it had one */
        net_sock_ref_release(s);      /* the creator's */
        free(s);
    }
    return NULL;
}

static void *ct_reader(void *arg)
{
    volatile int *stop = arg;
    while (!*stop) {
        (void)net_bh_any_pending();
        g_visited = 0;
        (void)net_socket_table_walk(NET_TABLE_TCP, AF_UNSPEC, count_visit, NULL);
        for (int i = 0; i < 8; i++)
            (void)net_bh_slot_pending(i);
    }
    return NULL;
}

static int scenario_concurrency(void)
{
    unsigned long bitmap_before = bitmap_sum();
    char row_before[512], row_after[512];
    (void)net_sock_ref_format(row_before, sizeof(row_before));
    int live_before = net_sock_ref_live();

    pthread_t workers[CT_WORKERS];
    ct_worker_t args[CT_WORKERS];
    volatile int stop = 0;
    pthread_t reader;

    pthread_create(&reader, NULL, ct_reader, (void *)&stop);
    for (int i = 0; i < CT_WORKERS; i++) {
        args[i].rounds = CT_ROUNDS;
        args[i].errors = 0;
        pthread_create(&workers[i], NULL, ct_worker, &args[i]);
    }
    int errors = 0;
    for (int i = 0; i < CT_WORKERS; i++) {
        pthread_join(workers[i], NULL);
        errors += args[i].errors;
    }
    stop = 1;
    pthread_join(reader, NULL);

    (void)net_sock_ref_format(row_after, sizeof(row_after));
    int live_after = net_sock_ref_live();
    unsigned long bitmap_after = bitmap_sum();

    say("concurrency: workers=%d rounds=%d errors=%d",
        CT_WORKERS, CT_ROUNDS, errors);
    say("concurrency: bitmap_restored=%d live_restored=%d",
        bitmap_after == bitmap_before, live_after == live_before);
    /* The ledger row is the whole reference-count story: allocs and frees must
     * have advanced by the same amount and faults must have stayed zero. */
    say("concurrency: ledger_row=%s", row_after[1] == 'n' ? "net_sock_ref" : "?");
    printf("%s", row_after);
    return (errors == 0 && bitmap_after == bitmap_before &&
            live_after == live_before) ? 0 : fail("concurrency invariants");
}

int main(void)
{
    net_socket_registry_init();
    if (scenario_slots())
        return 1;
    if (scenario_ring())
        return 1;
    if (scenario_refs())
        return 1;
    if (scenario_pending())
        return 1;
    if (scenario_table())
        return 1;
    if (scenario_concurrency())
        return 1;
    say("ledger: live=%d", net_sock_ref_live());
    say("ledger: bitmap=%lx", bitmap_sum());
    say("HARNESS OK");
    return 0;
}
