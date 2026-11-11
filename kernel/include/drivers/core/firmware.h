/* SPDX-License-Identifier: Apache-2.0 */
#ifndef A20_DRIVER_FIRMWARE_H
#define A20_DRIVER_FIRMWARE_H

#include "core/types.h"

#define DRIVER_FIRMWARE_NAME_MAX 255
#define DRIVER_FIRMWARE_SIZE_MAX (64UL * 1024 * 1024)

struct vfile;
struct driver_firmware_source;
struct driver_firmware;

/* Synchronous, task-context provider. open returns 0 and an owned stream plus
 * its exact byte length; on failure it cleans up its own partial resources.
 * read returns bytes read, 0 at EOF, or negative errno, at the supplied offset.
 * Short reads are supported. close is called exactly once after a successful
 * open, including allocation/read/size failures. Independent streams may be
 * used concurrently. Names are already validated canonical relative paths.
 * The provider and its module must remain resident until release returns. */
struct driver_firmware_ops {
    int (*open)(void *context, const char *name, void **stream, uint64_t *size);
    int (*read)(void *context, void *stream, uint64_t offset,
                void *buffer, size_t count);
    void (*close)(void *context, void *stream);
    void (*release)(void *context);
};

/* Copies ops. Success transfers one context ownership reference; failure
 * leaves context ownership with the caller. All final puts are task context. */
int driver_firmware_source_create(const struct driver_firmware_ops *ops,
                                  void *context,
                                  struct driver_firmware_source **out);
void driver_firmware_source_get(struct driver_firmware_source *source);
void driver_firmware_source_put(struct driver_firmware_source *source);

/* Captures an independent reference to an already-open trusted directory.
 * No caller fd, cwd, root, or namespace is consulted during later loads.
 * Walks actual children on this mount only; symlinks and special files are
 * rejected. Provision real, uncompressed files beneath this directory.
 * Kernel access deliberately bypasses current-task DAC checks. The caller
 * must trust the directory and its writers; this is not a path sandbox. */
int driver_firmware_source_create_vfs(struct vfile *directory,
                                      struct driver_firmware_source **out);

/* Blocking task context, outside spinlocks. Caller holds a source reference.
 * max_size must be nonzero; the hard cap also applies.
 * No empty/dot/dot-dot components, leading/trailing/repeated slash, or names
 * longer than DRIVER_FIRMWARE_NAME_MAX. Copies all bytes before publishing *out.
 * Failure sets *out=NULL and releases partial resources. No fallback source,
 * decompression, userspace helper, cancellation, or signature verification.
 * Deploy immutable files: copying does not lock out concurrent modification. */
int driver_firmware_load(struct driver_firmware_source *source, const char *name,
                          size_t max_size, struct driver_firmware **out);
/* Independent copy for built-in firmware. Does not retain the input buffer. */
int driver_firmware_from_memory(const void *data, size_t size, size_t max_size,
                                 struct driver_firmware **out);
void driver_firmware_get(struct driver_firmware *firmware);
void driver_firmware_put(struct driver_firmware *firmware);
/* Immutable CPU RAM, valid while caller holds an image reference. This is
 * not DMA memory; hardware upload, format and signature checks are separate. */
const void *driver_firmware_data(const struct driver_firmware *firmware);
size_t driver_firmware_size(const struct driver_firmware *firmware);

#endif
