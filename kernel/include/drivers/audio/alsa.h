#ifndef _DRIVERS_AUDIO_ALSA_H
#define _DRIVERS_AUDIO_ALSA_H

/*
 * Minimal Linux ALSA-compatible layer (kernel/drivers/audio/alsa.c).
 *
 * Exposes the standard ALSA PCM/control ioctls on /dev/snd/controlC0 and
 * /dev/snd/pcmC0D0p|c mapped onto the A20 audio_dev_ops_t backend, so
 * ALSA-based userland can open a PCM playback/capture stream.  Supports the
 * ioctl path (SNDRV_PCM_IOCTL_WRITEI_FRAMES / READI_FRAMES); the mmap
 * playback path is provided through a kernel ring buffer.
 */

#include "core/types.h"

/* ALSA PCM ioctls (Linux ABI). */
#define SNDRV_PCM_IOCTL_HW_FREE      0x00004112
#define SNDRV_PCM_IOCTL_PREPARE      0x00004140
#define SNDRV_PCM_IOCTL_RESET        0x00004141
#define SNDRV_PCM_IOCTL_START        0x00004142
#define SNDRV_PCM_IOCTL_DROP         0x00004143
#define SNDRV_PCM_IOCTL_DRAIN        0x00004144
#define SNDRV_PCM_IOCTL_PAUSE        0x40044145
#define SNDRV_PCM_IOCTL_TSTAMP       0x40044102
#define SNDRV_PCM_IOCTL_HW_PARAMS    0xc1504111
#define SNDRV_PCM_IOCTL_SW_PARAMS    0xc0884113
#define SNDRV_PCM_IOCTL_STATUS       0x80884120
#define SNDRV_PCM_IOCTL_WRITEI_FRAMES 0x40184150
#define SNDRV_PCM_IOCTL_READI_FRAMES 0x80184151

/* ALSA control ioctls. */
#define SNDRV_CTL_IOCTL_PVERSION     0x80045500
#define SNDRV_CTL_IOCTL_CARD_INFO    0x81a85501
#define SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE 0xc0045511
#define SNDRV_CTL_IOCTL_PCM_INFO     0xc1105512

/* Create the ALSA PCM node backend (playback=1 or capture=0). */
int alsa_pcm_create_file(int playback);

/* Create the ALSA control node backend. */
int alsa_control_create_file(void);

/* vfile variants used by the /dev/snd/* devfs open path. */
struct vfile *alsa_pcm_create_vfile(int playback);
struct vfile *alsa_control_create_vfile(void);

#endif /* _DRIVERS_AUDIO_ALSA_H */
