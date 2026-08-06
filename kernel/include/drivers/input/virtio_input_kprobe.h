/*
 * virtio_input_kprobe.h — kernel placement of the dual-placement
 * virtio-input driver (probe shell).
 */
#ifndef _DRIVERS_INPUT_VIRTIO_INPUT_KPROBE_H
#define _DRIVERS_INPUT_VIRTIO_INPUT_KPROBE_H

/* Probe the dual-placement virtio-input slot read-only and log the
 * device identity.  Silent when no device occupies the slot. */
void virtio_input_kprobe(void);

#endif
