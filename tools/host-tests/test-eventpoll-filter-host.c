#include <assert.h>
#include <stdint.h>
#include <unistd.h>

#include "../kernel/fs/readiness.c"

typedef struct rust_readiness_state {
    uint32_t mode;
    int16_t active;
    uint64_t generation;
    bool enabled;
} rust_readiness_state_t;

_Static_assert(sizeof(rust_readiness_state_t) == 24,
               "Rust/C readiness state ABI mismatch");

extern short a20_eventpoll_filter_ready_rust(short raw, uint64_t generation,
                                             void *state);

typedef struct epoll_event_abi {
    uint32_t events;
    uint64_t data;
} epoll_event_abi_t;

typedef struct epoll_item_abi {
    int32_t fd;
    int32_t registered;
    uint64_t identity;
    epoll_event_abi_t ev;
    rust_readiness_state_t state;
} epoll_item_abi_t;

extern int a20_eventpoll_add_rust(void *items, int *count, int fd,
                                  uint64_t identity, uint32_t events,
                                  uint64_t data);
extern int a20_eventpoll_mod_rust(void *items, int fd, uint64_t identity,
                                  uint32_t events, uint64_t data);

_Static_assert(sizeof(epoll_item_abi_t) == 56,
               "Rust/C epoll item ABI mismatch");

static short poll_result;
static uint64_t queue_generation;

uint64_t wait_queue_generation(wait_queue_t *queue)
{
    (void)queue;
    return queue_generation;
}

bool fdtable_current_matches_file(int fd, vfile_t *vf, uint64_t identity)
{
    (void)fd;
    (void)vf;
    (void)identity;
    return false;
}

int vfs_poll_file(vfile_t *vf, short events)
{
    (void)vf;
    (void)events;
    return poll_result;
}

static readiness_interest_t interest(vfile_t *file,
                                     rust_readiness_state_t *state)
{
    return (readiness_interest_t){
        .fd = -1,
        .vfile = file,
        .events = POLLIN,
        .flags = READINESS_F_GLOBAL_FD,
        .expected_identity = file->identity,
        .filter = a20_eventpoll_filter_ready_rust,
        .filter_arg = state,
    };
}

static uint64_t setup(vfile_t *file, readiness_slot_t *slot,
                      wait_queue_t *queue)
{
    file->identity = 7;
    slot->file = file;
    slot->identity = file->identity;
    slot->source_count = 1;
    slot->sources[0].queue = queue;
    return readiness_slot_generation(slot);
}

int main(void)
{
    vfile_t file = {0};
    readiness_slot_t slot = {0};
    wait_queue_t queue = {0};
    queue_generation = 1;
    uint64_t gen = setup(&file, &slot, &queue);
    rust_readiness_state_t edge = {
        .mode = 1,
        .active = 0,
        .generation = 0,
        .enabled = true,
    };
    readiness_interest_t item = interest(&file, &edge);
    poll_result = POLLIN;

    assert(readiness_scan(&item, &slot, 1, 1) == 1);
    assert(item.revents == POLLIN && edge.active == POLLIN &&
           edge.generation == gen);
    assert(readiness_scan(&item, &slot, 1, 1) == 0);
    assert(item.revents == 0); /* No new edge at the same generation. */

    queue_generation++;
    gen = readiness_slot_generation(&slot);
    assert(readiness_scan(&item, &slot, 1, 1) == 1);
    assert(item.revents == POLLIN && edge.generation == gen);

    poll_result = 0;
    assert(readiness_scan(&item, &slot, 1, 1) == 0);
    assert(edge.active == 0);
    poll_result = POLLIN;
    assert(readiness_scan(&item, &slot, 1, 1) == 1);
    assert(item.revents == POLLIN); /* A zero sample rearms the edge. */

    /* Run the same generation sequence through the retained C control. */
    readiness_state_t c_edge;
    readiness_state_init(&c_edge, READINESS_MODE_EDGE);
    item = interest(&file, NULL);
    item.filter = NULL;
    item.filter_arg = NULL;
    item.state = &c_edge;
    assert(readiness_scan(&item, &slot, 1, 1) == 1);
    assert(readiness_scan(&item, &slot, 1, 1) == 0);
    queue_generation++;
    gen = readiness_slot_generation(&slot);
    assert(readiness_scan(&item, &slot, 1, 1) == 1);
    poll_result = 0;
    assert(readiness_scan(&item, &slot, 1, 1) == 0 && c_edge.active == 0);
    poll_result = POLLIN;
    assert(readiness_scan(&item, &slot, 1, 1) == 1);

    readiness_state_t c_oneshot;
    readiness_state_init(&c_oneshot,
                         READINESS_MODE_EDGE | READINESS_MODE_ONESHOT);
    item.state = &c_oneshot;
    assert(readiness_scan(&item, &slot, 1, 1) == 1);
    assert(!c_oneshot.enabled);
    readiness_state_rearm(&c_oneshot,
                          READINESS_MODE_EDGE | READINESS_MODE_ONESHOT);
    assert(readiness_scan(&item, &slot, 1, 1) == 1);

    epoll_item_abi_t table[1024] = {0};
    int table_count = 0;
    assert(a20_eventpoll_add_rust(table, &table_count, 12, file.identity,
                                  (1u << 30) | 1u, 0x1122) == 0);
    assert(table_count == 1 && table[0].registered == 1);
    item = interest(&file, &table[0].state);
    assert(readiness_scan(&item, &slot, 1, 1) == 1);
    assert(item.revents == POLLIN && !table[0].state.enabled);
    assert(readiness_scan(&item, &slot, 1, 1) == 0);
    assert(a20_eventpoll_mod_rust(table, 12, file.identity,
                                  (1u << 30) | 1u, 0x3344) == 0);
    assert(table[0].state.enabled && table[0].state.active == 0 &&
           table[0].ev.data == 0x3344);
    assert(readiness_scan(&item, &slot, 1, 1) == 1);

    /* A filtered ET duplicate does not exhaust max_ready before a later LT. */
    rust_readiness_state_t duplicate = {
        .mode = 1,
        .active = POLLIN,
        .generation = gen,
        .enabled = true,
    };
    rust_readiness_state_t level = {
        .mode = 0,
        .active = 0,
        .generation = 0,
        .enabled = true,
    };
    vfile_t files[2] = {0};
    readiness_slot_t slots[2] = {0};
    files[0].identity = 8;
    files[1].identity = 9;
    slots[0] = slot;
    slots[1] = slot;
    slots[0].file = &files[0];
    slots[0].identity = files[0].identity;
    slots[1].file = &files[1];
    slots[1].identity = files[1].identity;
    readiness_interest_t items[2] = {
        interest(&files[0], &duplicate),
        interest(&files[1], &level),
    };
    assert(readiness_scan(items, slots, 2, 1) == 1);
    assert(items[0].revents == 0 && items[1].revents == POLLIN);

    item = interest(&file, &level);
    item.expected_identity++;
    assert(readiness_scan(&item, &slot, 1, 1) == 1);
    assert(item.revents == POLLNVAL); /* Invalid interests still reach Rust. */

    static const char pass[] = "eventpoll C/Rust LT/ET/ONESHOT host: PASS\n";
    (void)write(1, pass, sizeof(pass) - 1);
    return 0;
}
