#ifndef _DRIVERS_AUDIO_BACKEND_H
#define _DRIVERS_AUDIO_BACKEND_H

#include "core/types.h"

/* Internal calls through audio_dev_ops.ioctl, never userspace ioctl payloads.
 * The PCM adapter holds the class binding while these borrowed pointers are
 * live. Existing driver module vtables and their ABI do not change. */
#define AUDIO_BACKEND_QUERY          0xa20b0001UL
#define AUDIO_BACKEND_START          0xa20b0002UL
#define AUDIO_BACKEND_WRITE_NONBLOCK 0xa20b0003UL

typedef struct audio_backend_status {
    uint64_t capacity_bytes;
    uint64_t queued_bytes;
    uint32_t frame_bytes;
    uint32_t running;
    uint32_t underruns;
} audio_backend_status_t;

typedef struct audio_backend_write {
    const void *data;
    size_t bytes;
    /* Zero leaves a prepared stream stopped. Otherwise start atomically once
     * this submission accepted at least this many bytes. */
    size_t start_after_bytes;
} audio_backend_write_t;

#endif
