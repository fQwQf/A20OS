#ifndef _FS_VFS_VFILE_LIFECYCLE_C_H
#define _FS_VFS_VFILE_LIFECYCLE_C_H

#include <stdint.h>

/* Reference transition model used when VFS_LIFECYCLE_IMPL=c. */
static inline int a20_vfile_close_begin_token_c(uint32_t *state,
                                                int token_capable)
{
    if (!state) return -22;
    if (*state == 0 || *state == 1) { *state = 4; return 0; }
    /* AMBIGUOUS: a backend carrying an idempotent close token may retry an
     * un-acked sent CLOSE; without the capability it stays sticky -EIO. */
    if (*state == 2) {
        if (token_capable) { *state = 4; return 0; }
        return -5;
    }
    if (*state == 3) return -114;
    if (*state == 4) return -16;
    return -22;
}

static inline int a20_vfile_close_begin_c(uint32_t *state)
{ return a20_vfile_close_begin_token_c(state, 0); }

static inline int a20_vfile_close_finish_c(uint32_t *state, int status,
                                           int sent, int acked,
                                           int retry_safe)
{
    if (!state) return -22;
    if (*state != 4) return -16;
    if (!status && acked) { *state = 3; return 0; }
    if (!sent && retry_safe) { *state = 1; return status < 0 ? status : -5; }
    *state = 2;
    return status < 0 ? status : -5;
}

static inline int a20_vfile_close_is_done_c(const uint32_t *state)
{ return state && *state == 3; }

#endif
