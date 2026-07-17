#ifndef _VIRTIO_SCSI_H
#define _VIRTIO_SCSI_H

#include "drivers/block/block_dev.h"

/* VirtIO device ID assigned to the SCSI host device. */
#define VIRTIO_ID_SCSI 8

block_dev_t *virtio_scsi_get_dev(int index);
int virtio_scsi_ready(int index);

#endif /* _VIRTIO_SCSI_H */
