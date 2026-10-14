#ifndef _LINUX_SCM_RIGHTS_H
#define _LINUX_SCM_RIGHTS_H

#include "core/types.h"

#define LINUX_SCM_RIGHTS_MAX_FDS 16
#define LINUX_SCM_RIGHTS_NO_RIGHTS ((int64_t)(-9223372036854775807LL - 1))

typedef struct linux_scm_recv_txn {
    int installed[LINUX_SCM_RIGHTS_MAX_FDS];
    void *pinned_files[LINUX_SCM_RIGHTS_MAX_FDS];
    size_t count;
    uint32_t active;
} linux_scm_recv_txn_t;

int64_t a20_linux_scm_rights_send(
    const uint8_t *control, size_t control_len, void *ctx,
    int (*acquire)(int fd, void *ctx, void **file_out),
    void (*release)(void *ctx, void *file),
    int64_t (*enqueue)(void *ctx, void **files, int count));

int a20_linux_scm_rights_recv_begin(
    void **files, int count, size_t control_capacity, size_t used,
    int cloexec, void *ctx,
    int (*install)(void *ctx, void *file, int cloexec, void **pinned_out),
    void (*release)(void *ctx, void *file),
    linux_scm_recv_txn_t *txn, int *out_fds, size_t *out_count);
void a20_linux_scm_rights_recv_commit(
    linux_scm_recv_txn_t *txn, void *ctx,
    void (*release)(void *ctx, void *file));
void a20_linux_scm_rights_recv_abort(
    linux_scm_recv_txn_t *txn, void *ctx,
    void (*close_if_file)(void *ctx, int fd, void *expected_file),
    void (*release)(void *ctx, void *file));

int a20_linux_scm_queue_take(void **queued_files, int *queued_count,
                             void **recv_files, int *recv_count,
                             int capacity);
int a20_linux_scm_queue_attach(void **source_files, int count,
                               void **queued_files, int queued_capacity);
void a20_linux_scm_queue_drop(
    void **queued_files, int *queued_count, void *ctx,
    void (*release)(void *ctx, void *file));

#endif
