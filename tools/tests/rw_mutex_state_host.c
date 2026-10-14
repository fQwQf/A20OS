#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct rw_mutex_state {
    uint32_t readers;
    uint32_t waiting_writers;
    int32_t writer;
    void *owner;
} rw_mutex_state_t;

void a20_rw_mutex_state_init_rust(rw_mutex_state_t *);
int a20_rw_mutex_read_admit_rust(rw_mutex_state_t *);
int a20_rw_mutex_read_release_rust(rw_mutex_state_t *);
void a20_rw_mutex_writer_wait_rust(rw_mutex_state_t *);
int a20_rw_mutex_write_admit_rust(rw_mutex_state_t *, void *);
int a20_rw_mutex_write_release_rust(rw_mutex_state_t *);

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL rw-mutex host: %s\n", message);
        exit(1);
    }
}

int main(void)
{
    rw_mutex_state_t state = { .readers = 99, .waiting_writers = 99,
                               .writer = 1, .owner = (void *)1 };
    int writer_a, writer_b;
    a20_rw_mutex_state_init_rust(&state);
    check(!state.readers && !state.waiting_writers && !state.writer &&
          !state.owner, "initial state");

    check(a20_rw_mutex_read_admit_rust(&state), "first reader admitted");
    check(a20_rw_mutex_read_admit_rust(&state), "concurrent reader admitted");
    check(state.readers == 2, "reader count owns both admissions");
    a20_rw_mutex_writer_wait_rust(&state);
    check(state.waiting_writers == 1, "writer enters waiting set");
    check(!a20_rw_mutex_read_admit_rust(&state),
          "waiting writer blocks reader barging");
    check(!a20_rw_mutex_read_release_rust(&state),
          "first reader release does not wake writer");
    check(a20_rw_mutex_read_release_rust(&state),
          "last reader release wakes waiting writer");
    check(a20_rw_mutex_write_admit_rust(&state, &writer_a),
          "writer admitted after readers drain");
    check(state.writer && state.owner == &writer_a &&
          state.waiting_writers == 0, "writer ownership and queue count");
    check(!a20_rw_mutex_read_admit_rust(&state),
          "active writer blocks readers");

    a20_rw_mutex_writer_wait_rust(&state);
    check(state.waiting_writers == 1, "second writer waits behind owner");
    check(a20_rw_mutex_write_release_rust(&state),
          "writer release prefers another queued writer");
    check(!state.writer && !state.owner, "writer owner cleared on release");
    check(a20_rw_mutex_write_admit_rust(&state, &writer_b),
          "next writer consumes waiting slot");
    check(state.writer && state.owner == &writer_b &&
          state.waiting_writers == 0, "second writer state");
    check(!a20_rw_mutex_write_release_rust(&state),
          "last writer release permits readers");
    check(a20_rw_mutex_read_admit_rust(&state),
          "reader enters after writer phase");
    check(!a20_rw_mutex_read_release_rust(&state),
          "reader drains without queued writer");
    check(!state.readers && !state.writer && !state.owner,
          "all state drains to idle");

    puts("RW_MUTEX_HOST: PASS writer-preference and ownership transitions");
    return 0;
}
