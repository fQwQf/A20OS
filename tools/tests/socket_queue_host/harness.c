/* Fault-injection harness for the exact production kernel/net/socket/socket_queue.c.
 * C and Rust selector runs compile this same TU and compare the emitted state. */
#include "net/socket/socket_internal.h"
#include "net/socket_side.h"
#include "mm/objcache.h"
#include "mm/slab.h"
#include "lwip/pbuf.h"
#include "/usr/include/stdlib.h"
#include "/usr/include/string.h"
#include <unistd.h>

static int fail_cache;
static int fail_heap;
static int fail_pbuf_copy;
static int short_pbuf_copy;
static int file_puts;
static int obj_allocs;
static int obj_frees;

void *obj_cache_alloc_zero(obj_cache_t *cache)
{
    if (fail_cache) return NULL;
    void *p = calloc(1, cache->obj_size);
    if (p) obj_allocs++;
    return p;
}
void obj_cache_free(obj_cache_t *cache, void *p)
{
    (void)cache;
    if (p) obj_frees++;
    free(p);
}
void *kmalloc(size_t n) { return fail_heap ? NULL : malloc(n); }
void kfree(void *p) { free(p); }
void vfs_put_file(vfile_t *f) { if (f) file_puts++; }
task_t *proc_current(void) { return NULL; }
int net_notconn(net_notconn_reason_t why) { (void)why; return -ENOTCONN; }
u16_t pbuf_copy_partial(const struct pbuf *p, void *dst, u16_t n, u16_t off)
{
    if (fail_pbuf_copy || !p || off > p->tot_len || n > p->tot_len - off)
        return 0;
    u16_t copied = short_pbuf_copy && n ? (u16_t)(n - 1) : n;
    memcpy(dst, (const uint8_t *)p->payload + off, copied);
    return copied;
}

static int fail(const char *s)
{
    (void)write(2, s, strlen(s));
    return 1;
}
#define CHECK(c, s) do { if (!(c)) return fail(s); } while (0)

static int empty(const net_socket_t *s)
{
    return !s->rx_head && !s->rx_tail && s->rx_count == 0 && s->rxq_tally == 0;
}

int main(void)
{
    net_socket_t s = {0};
    s.type = SOCK_STREAM;
    const char payload[] = "abcdefgh";

    /* Keep a real queued SCM ref in place while an allocation attempt fails;
     * failure must neither mutate the entry nor release/steal its ref. */
    CHECK(net_enqueue_msg_locked(&s, payload, 1, NULL, 0) == 1,
          "baseline-enqueue\n");
    s.rx_head->scm_files[0] = (vfile_t *)(uintptr_t)0x1010;
    s.rx_head->scm_nfiles = 1;
    net_msg_t *baseline_head = s.rx_head;
    net_msg_t *baseline_tail = s.rx_tail;
    int baseline_count = s.rx_count;
    uint64_t baseline_tally = s.rxq_tally;
    int baseline_puts = file_puts;

    /* Cache and spill allocation failures must precede publication. */
    fail_cache = 1;
    CHECK(net_enqueue_msg_locked(&s, payload, sizeof(payload), NULL, 0) == -EAGAIN,
          "cache-failure-result\n");
    fail_cache = 0;
    CHECK(s.rx_head == baseline_head && s.rx_tail == baseline_tail &&
          s.rx_count == baseline_count && s.rxq_tally == baseline_tally &&
          s.rx_head->scm_nfiles == 1 &&
          s.rx_head->scm_files[0] == (vfile_t *)(uintptr_t)0x1010 &&
          file_puts == baseline_puts, "cache-failure-mutated-queue-or-refs\n");
    net_msg_t *baseline_list = net_rxq_detach_locked(&s);
    net_msg_free(baseline_list);
    CHECK(file_puts == baseline_puts + 1 && empty(&s),
          "baseline-scm-ref-finalization\n");
    fail_heap = 1;
    CHECK(net_enqueue_msg_locked(&s, payload, 2048, NULL, 0) == -EAGAIN,
          "spill-failure-result\n");
    fail_heap = 0;
    CHECK(empty(&s), "spill-failure-mutated-queue\n");

    /* Pbuf copy failure also frees the unpublished message without changing
     * linked state. The pbuf is a borrowed source in the production entry. */
    struct pbuf p = {0};
    p.payload = (void *)payload;
    p.tot_len = sizeof(payload);
    fail_pbuf_copy = 1;
    CHECK(net_enqueue_msg_locked_pbuf(&s, &p, 0, sizeof(payload), NULL, 0, NULL) == -EAGAIN,
          "pbuf-copy-failure-result\n");
    fail_pbuf_copy = 0;
    CHECK(empty(&s), "pbuf-copy-failure-mutated-queue\n");
    short_pbuf_copy = 1;
    CHECK(net_enqueue_msg_locked_pbuf(&s, &p, 0, 8, NULL, 0, NULL) == -EAGAIN,
          "pbuf-short-copy-result\n");
    short_pbuf_copy = 0;
    CHECK(empty(&s), "pbuf-short-copy-mutated-queue\n");
    CHECK(net_enqueue_msg_locked_pbuf(&s, &p, 1, 3, NULL, 0, NULL) == 3,
          "pbuf-success-result\n");
    char pbuf_out[4] = {0};
    CHECK(net_dequeue_msg_locked(&s, pbuf_out, sizeof(pbuf_out), NULL, NULL) == 3 &&
          !memcmp(pbuf_out, "bcd", 3), "pbuf-success-payload\n");
    CHECK(net_enqueue_msg_locked_pbuf(&s, &p, 0, 0, NULL, 0, NULL) == 0,
          "pbuf-zero-copy-result\n");
    CHECK(net_dequeue_msg_locked(&s, pbuf_out, sizeof(pbuf_out), NULL, NULL) == 0 &&
          empty(&s), "pbuf-zero-copy-state\n");

    /* Successful C production enqueue/dequeue, partial stream read and tally
     * repair after deliberate stale accounting. */
    CHECK(net_enqueue_msg_locked(&s, payload, 8, NULL, 0) == 8,
          "enqueue-result\n");
    CHECK(s.rx_count == 1 && net_rxq_bytes_locked(&s) == 8,
          "enqueue-accounting\n");
    s.rxq_tally = 0; /* trigger the production count-mismatch rebuild */
    CHECK(net_rxq_bytes_locked(&s) == 8, "stale-tally-repair\n");
    char out[16] = {0};
    CHECK(net_dequeue_msg_locked(&s, out, 3, NULL, NULL) == 3,
          "partial-read-result\n");
    CHECK(!memcmp(out, "abc", 3) && s.rx_count == 1 &&
          net_rxq_bytes_locked(&s) == 5, "partial-read-accounting\n");
    CHECK(net_dequeue_msg_locked(&s, out, sizeof(out), NULL, NULL) == 5,
          "remaining-read-result\n");
    CHECK(!memcmp(out, "defgh", 5) && !s.rx_head && !s.rx_tail && s.rx_count == 0 &&
          net_rxq_bytes_locked(&s) == 0 && empty(&s), "full-read-accounting\n");

    /* Fill the real queue to its production limit; the rejected message must
     * leave head/tail/count/tally byte-for-byte stable. */
    s.type = SOCK_DGRAM;
    for (int i = 0; i < NET_MAX_QUEUE; ++i)
        CHECK(net_enqueue_msg_locked(&s, payload, 1, NULL, 0) == 1,
              "queue-fill-result\n");
    net_msg_t *head = s.rx_head;
    net_msg_t *tail = s.rx_tail;
    int count = s.rx_count;
    uint64_t tally = s.rxq_tally;
    CHECK(net_enqueue_msg_locked(&s, payload, 1, NULL, 0) == -EAGAIN,
          "full-queue-result\n");
    CHECK(s.rx_head == head && s.rx_tail == tail && s.rx_count == count &&
          s.rxq_tally == tally, "full-queue-mutated-state\n");

    /* Put two owned file refs on the actual head message, detach via the
     * production teardown transition, then release the list through the real
     * net_msg_free() path. Each queued ref must be finalized exactly once. */
    head->scm_files[0] = (vfile_t *)(uintptr_t)0x1110;
    head->scm_files[1] = (vfile_t *)(uintptr_t)0x2220;
    head->scm_nfiles = 2;
    net_msg_t *list = net_rxq_detach_locked(&s);
    CHECK(list == head && empty(&s), "detach-reset-state\n");
    int before = file_puts;
    while (list) {
        net_msg_t *next = list->next;
        list->next = NULL;
        net_msg_free(list);
        list = next;
    }
    CHECK(file_puts - before == 2, "scm-ref-finalization-count\n");
    CHECK(obj_allocs == obj_frees, "message-allocation-balance\n");
    const char *pass = "socket-queue-production-harness PASS\n";
    (void)write(1, pass, strlen(pass));
    return 0;
}
