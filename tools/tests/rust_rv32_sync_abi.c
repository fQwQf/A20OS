#include <stddef.h>

#include "core/sync.h"

_Static_assert(offsetof(wait_queue_entry_t, next) == 0, "entry.next");
_Static_assert(offsetof(wait_queue_entry_t, prev) == 4, "entry.prev");
_Static_assert(offsetof(wait_queue_entry_t, queue) == 8, "entry.queue");
_Static_assert(offsetof(wait_queue_entry_t, task) == 12, "entry.task");
_Static_assert(offsetof(wait_queue_entry_t, wait_seq) == 16, "entry.wait_seq");
_Static_assert(offsetof(wait_queue_entry_t, flags) == 24, "entry.flags");
_Static_assert(offsetof(wait_queue_entry_t, key) == 28, "entry.key");
_Static_assert(offsetof(wait_queue_entry_t, priv) == 32, "entry.priv");
_Static_assert(offsetof(wait_queue_entry_t, linked) == 36, "entry.linked");
_Static_assert(_Alignof(wait_queue_entry_t) == 8, "entry.align");
_Static_assert(sizeof(wait_queue_entry_t) == 40, "entry.size");

_Static_assert(offsetof(proc_wait_token_t, task) == 0, "token.task");
_Static_assert(offsetof(proc_wait_token_t, seq) == 8, "token.seq");
_Static_assert(offsetof(proc_wait_token_t, prepare_error) == 16, "token.error");
_Static_assert(_Alignof(proc_wait_token_t) == 8, "token.align");
_Static_assert(sizeof(proc_wait_token_t) == 24, "token.size");

_Static_assert(offsetof(rw_mutex_state_t, readers) == 0, "rw.readers");
_Static_assert(offsetof(rw_mutex_state_t, waiting_writers) == 4, "rw.waiting_writers");
_Static_assert(offsetof(rw_mutex_state_t, writer) == 8, "rw.writer");
_Static_assert(offsetof(rw_mutex_state_t, owner) == 12, "rw.owner");
_Static_assert(sizeof(rw_mutex_state_t) == 16, "rw.size");

_Static_assert(offsetof(mutex_t, depth) == offsetof(mutex_t, locked) + 4,
               "mutex.depth");
_Static_assert(offsetof(mutex_t, owner) == offsetof(mutex_t, depth) + 4,
               "mutex.owner");

int a20_rv32_sync_c_layout_probe(void)
{
    return (int)(sizeof(wait_queue_entry_t) + sizeof(proc_wait_token_t) +
                 sizeof(rw_mutex_state_t));
}
