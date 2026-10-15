#ifndef A20_UFSD_RUST_ABI_H
#define A20_UFSD_RUST_ABI_H

#include <stdint.h>
#include "fs/ufs_proto.h"

enum { A20_UFSD_BACKEND_FAT = 1, A20_UFSD_BACKEND_VNFS = 2 };

int64_t a20_ufsd_startup(uint32_t *stdout_handle);
int64_t a20_ufsd_set_block_index(int32_t block_index);
int64_t a20_ufsd_channel_create(uint32_t endpoints[2]);
int64_t a20_ufsd_path_create(const char *path, uint32_t path_len);
int64_t a20_ufsd_serve(uint32_t server_endpoint, int32_t block_index,
                       const char *path, uint32_t path_len, uint32_t flags);
int64_t a20_ufsd_umount(const char *path, uint32_t path_len);
int64_t a20_ufsd_capacity(int32_t block_index, uint64_t *sectors);
int64_t a20_ufsd_event_queue_create(uint32_t *queue);
int64_t a20_ufsd_event_watch(uint32_t queue, uint32_t target,
                             uint64_t mask, uint64_t tag);
int64_t a20_ufsd_event_wait(uint32_t queue);
int64_t a20_ufsd_recv(uint32_t endpoint, uint8_t *bytes,
                      uint32_t capacity, uint32_t *length);
int64_t a20_ufsd_send(uint32_t endpoint, const ufs_resp_hdr_t *response);
int64_t a20_ufsd_send_bytes(uint32_t endpoint, const uint8_t *bytes, uint32_t length);
int64_t a20_ufsd_close(uint32_t handle);
void a20_ufsd_log(uint32_t handle, const uint8_t *bytes, uint32_t length);
void a20_ufsd_exit(int32_t code);

int64_t a20_ufsd_backend_mount(uint32_t backend, const char *fstype);
int64_t a20_ufsd_backend_mount_readonly(uint32_t backend, const char *fstype);
int64_t a20_ufsd_backend_unmount(uint32_t backend);
int64_t a20_ufsd_backend_lookup(uint32_t backend, uint64_t ino,
                               const char *name, uint32_t name_len,
                               ufs_resp_hdr_t *response);
int64_t a20_ufsd_backend_getattr(uint32_t backend, uint64_t ino,
                                 ufs_resp_hdr_t *response);
int64_t a20_ufsd_backend_readdir(uint32_t backend, uint64_t ino, uint64_t skip,
                                 ufs_resp_hdr_t *response);
int64_t a20_ufsd_backend_create(uint32_t backend, uint64_t ino,
                                const char *name, uint32_t mode,
                                ufs_resp_hdr_t *response);
int64_t a20_ufsd_backend_mkdir(uint32_t backend, uint64_t ino,
                               const char *name, ufs_resp_hdr_t *response);
int64_t a20_ufsd_backend_unlink(uint32_t backend, uint64_t ino,
                                const char *name);
int64_t a20_ufsd_backend_rmdir(uint32_t backend, uint64_t ino,
                               const char *name);
int64_t a20_ufsd_backend_truncate(uint32_t backend, uint64_t ino, uint64_t size);
int64_t a20_ufsd_backend_read(uint32_t backend, uint64_t ino, uint64_t offset,
                              uint32_t count, ufs_resp_hdr_t *response);
int64_t a20_ufsd_backend_write(uint32_t backend, uint64_t ino, uint64_t offset,
                               const uint8_t *bytes, uint32_t count,
                               ufs_resp_hdr_t *response);
int64_t a20_ufsd_backend_rename(uint32_t backend, uint64_t old_dir,
                                const char *old_name, uint64_t new_dir,
                                const char *new_name, ufs_resp_hdr_t *response);
int64_t a20_ufsd_backend_sync(uint32_t backend);
void a20_ufsd_backend_statfs(uint32_t backend, ufs_resp_hdr_t *response);
uint8_t *a20_ufsd_payload_buffer(void);
uint32_t a20_ufsd_payload_capacity(void);

#endif
