#ifndef _FS_READINESS_H
#define _FS_READINESS_H

#include "core/sync.h"
#include "fs/vfs.h"

#define READINESS_MAX_FILE_SOURCES 4
#define READINESS_RETRY            (-4096)

#define READINESS_F_GLOBAL_FD      (1u << 0)

#define READINESS_MODE_EDGE        (1u << 0)
#define READINESS_MODE_ONESHOT     (1u << 1)

typedef struct readiness_source {
    wait_queue_t *queue;
    uintptr_t key;
    uint64_t deadline;
} readiness_source_t;

typedef struct readiness_state {
    uint32_t mode;
    short active;
    uint64_t generation;
    bool enabled;
} readiness_state_t;

typedef struct readiness_interest {
    int fd;
    short events;
    short revents;
    uint32_t flags;
    uintptr_t cookie;
    uint64_t expected_identity;
    readiness_state_t *state;
} readiness_interest_t;

typedef bool (*readiness_extra_ready_fn)(void *arg);

typedef struct readiness_extra {
    readiness_source_t source;
    readiness_extra_ready_fn ready;
    void *arg;
} readiness_extra_t;

void readiness_state_init(readiness_state_t *state, uint32_t mode);
void readiness_state_rearm(readiness_state_t *state, uint32_t mode);

/*
 * Query, subscribe, re-query and park once.  READINESS_RETRY asks the adapter
 * to rebuild its interest snapshot after an event or an internal source
 * deadline.  A non-negative result is the number of ready interests.
 */
int readiness_wait_once(readiness_interest_t *items, size_t count,
                        const readiness_extra_t *extra, size_t extra_count,
                        size_t max_ready, uint64_t deadline,
                        bool has_deadline);

#endif /* _FS_READINESS_H */
