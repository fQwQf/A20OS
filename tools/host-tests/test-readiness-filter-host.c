#include <assert.h>
#include <stdint.h>
#include <unistd.h>

/* Exercise the production scanner and its real callback dispatch. */
#include "../kernel/fs/readiness.c"

static short poll_results[4];
static size_t poll_index;
static size_t filter_calls;
static size_t drop_first;
static short seen_raw[4];
static uint64_t seen_generation[4];
static int seen_cookie[4];

uint64_t wait_queue_generation(wait_queue_t *queue)
{
    (void)queue;
    return 0;
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
    return poll_results[poll_index++];
}

static short test_filter(short raw, uint64_t generation, void *arg)
{
    size_t call = filter_calls++;
    seen_raw[call] = raw;
    seen_generation[call] = generation;
    seen_cookie[call] = *(int *)arg;
    return call < drop_first ? 0 : raw;
}

static void reset(size_t drop)
{
    poll_index = 0;
    filter_calls = 0;
    drop_first = drop;
    for (size_t i = 0; i < 4; i++) {
        poll_results[i] = POLLIN;
        seen_raw[i] = 0;
        seen_generation[i] = UINT64_MAX;
        seen_cookie[i] = 0;
    }
}

int main(void)
{
    vfile_t files[3] = {0};
    readiness_slot_t slots[3] = {0};
    readiness_interest_t items[3] = {0};
    int cookies[] = {17, 23, 31};
    for (size_t i = 0; i < 3; i++) {
        files[i].identity = i + 1;
        slots[i].file = &files[i];
        slots[i].identity = i + 1;
        items[i].fd = -1;
        items[i].flags = READINESS_F_GLOBAL_FD;
        items[i].events = POLLIN;
        items[i].vfile = &files[i];
        items[i].filter = test_filter;
        items[i].filter_arg = &cookies[i];
    }

    reset(1);
    assert(readiness_scan(items, slots, 2, 1) == 1);
    assert(filter_calls == 2); /* Dropped event does not consume max_ready. */
    assert(items[0].revents == 0 && items[1].revents == POLLIN);
    assert(seen_raw[0] == POLLIN && seen_generation[0] == 0 &&
           seen_cookie[0] == 17);
    assert(seen_raw[1] == POLLIN && seen_cookie[1] == 23);

    reset(0);
    assert(readiness_scan(items, slots, 3, 1) == 1);
    assert(filter_calls == 1); /* Stop after one delivered event. */

    reset(1);
    items[0].expected_identity = 99;
    assert(readiness_scan(items, slots, 2, 1) == 1);
    assert(filter_calls == 2);
    assert(seen_raw[0] == POLLNVAL && items[0].revents == 0);
    assert(seen_raw[1] == POLLIN && items[1].revents == POLLIN);
    items[0].expected_identity = 0;

    /* NULL hook preserves the existing state-machine behavior. */
    items[0].filter = NULL;
    readiness_state_t state = { .mode = 0, .active = 0,
                                .generation = 0, .enabled = true };
    items[0].state = &state;
    reset(0);
    assert(readiness_scan(items, slots, 1, 1) == 1);
    assert(items[0].revents == POLLIN && state.active == POLLIN);

    static const char pass[] = "readiness-filter-host: PASS\n";
    (void)write(1, pass, sizeof(pass) - 1);
    return 0;
}
