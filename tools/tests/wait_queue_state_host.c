#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct entry {
    struct entry *next;
    struct entry *prev;
    void *queue;
    void *task;
    uint64_t wait_seq;
    uint32_t flags;
    uintptr_t key;
    void *priv;
    bool linked;
} entry_t;

typedef struct queue {
    entry_t *head;
} queue_t;

bool a20_wait_queue_insert_rust(entry_t **, void *, entry_t *, void *,
                                uint64_t, uintptr_t);
void *a20_wait_queue_detach_rust(entry_t **, void *, entry_t *);
bool a20_wait_queue_requeue_rust(entry_t **, entry_t **, void *, void *,
                                 entry_t *);

static void check(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL wait-queue host: %s\n", message);
        exit(1);
    }
}

int main(void)
{
    queue_t a = {0}, b = {0};
    int tasks[3];
    int payloads[3];
    entry_t e[3] = {0};
    e[0].priv = &payloads[0];
    e[1].priv = &payloads[1];

    check(a20_wait_queue_insert_rust(&a.head, &a, &e[0], &tasks[0], 1, 7),
          "insert first entry");
    check(a20_wait_queue_insert_rust(&a.head, &a, &e[1], &tasks[1], 2, 8),
          "insert second entry");
    check(e[0].priv == &payloads[0] && e[1].priv == &payloads[1],
          "insert preserves caller-owned match payload");
    check(a.head == &e[1] && e[1].next == &e[0] &&
          e[0].prev == &e[1] && e[0].queue == &a && e[0].linked,
          "head insertion maintains both links and owner");
    check(!a20_wait_queue_insert_rust(&a.head, &a, &e[0], &tasks[2], 3, 9),
          "duplicate membership rejected");
    check(a20_wait_queue_requeue_rust(&a.head, &b.head, &a, &b, &e[0]),
          "move entry between queues");
    check(a.head == &e[1] && b.head == &e[0] && e[0].queue == &b &&
          e[0].prev == NULL && e[0].next == NULL,
          "requeue repairs both lists and publishes new owner");
    check(a20_wait_queue_detach_rust(&b.head, &b, &e[0]) == &tasks[0],
          "detach transfers task reference");
    check(!b.head && !e[0].queue && !e[0].task && !e[0].linked &&
          !e[0].next && !e[0].prev && e[0].wait_seq == 0 && e[0].key == 0 &&
          !e[0].priv,
          "detach clears every entry field before owner is cleared");
    check(a20_wait_queue_detach_rust(&a.head, &a, &e[1]) == &tasks[1],
          "detach original queue head");
    check(!a.head, "queue drains");
    puts("WAIT_QUEUE_HOST: PASS");
    return 0;
}
