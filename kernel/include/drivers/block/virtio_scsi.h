#ifndef _VIRTIO_SCSI_H
#define _VIRTIO_SCSI_H

#include "drivers/block/block_dev.h"

/* VirtIO device ID assigned to the SCSI host device. */
#define VIRTIO_ID_SCSI 8

/* A successful get pins the binding until block->release(block). Stop closes
 * new I/O and returns EBUSY while references remain; callers release after
 * their last cache/I/O use. ready() is a query and does not acquire a pin. */
block_dev_t *virtio_scsi_get_dev(int index);
int virtio_scsi_ready(int index);

#endif /* _VIRTIO_SCSI_H */
