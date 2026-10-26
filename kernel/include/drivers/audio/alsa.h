#ifndef _DRIVERS_AUDIO_ALSA_H
#define _DRIVERS_AUDIO_ALSA_H

/*
 * Minimal Linux ALSA-compatible layer (kernel/drivers/audio/alsa.c).
 *
 * Exposes the standard ALSA PCM/control ioctls on /dev/snd/controlC0 and
 * /dev/snd/pcmC0D0p|c mapped onto the A20 audio_dev_ops_t backend, so
 * ALSA-based userland can open a PCM playback/capture stream.  Supports the
 * ioctl path (SNDRV_PCM_IOCTL_WRITEI_FRAMES / READI_FRAMES). mmap PCM and
 * mmap status/control pages are not implemented; alsa-lib uses SYNC_PTR.
 */

#include "core/types.h"

/* ALSA PCM ioctls (Linux ABI); each number encodes its struct's size. */
#define SNDRV_PCM_IOCTL_PVERSION     0x80044100
#define SNDRV_PCM_IOCTL_INFO         0x81204101
#define SNDRV_PCM_IOCTL_TSTAMP       0x40044102
#define SNDRV_PCM_IOCTL_TTSTAMP      0x40044103
#define SNDRV_PCM_IOCTL_USER_PVERSION 0x40044104
#define SNDRV_PCM_IOCTL_HW_REFINE    0xc2604110
#define SNDRV_PCM_IOCTL_HW_PARAMS    0xc2604111
#define SNDRV_PCM_IOCTL_HW_FREE      0x00004112
#define SNDRV_PCM_IOCTL_SW_PARAMS    0xc0884113
#define SNDRV_PCM_IOCTL_STATUS       0x80984120
#define SNDRV_PCM_IOCTL_DELAY        0x80084121
#define SNDRV_PCM_IOCTL_SYNC_PTR     0xc0884123
#define SNDRV_PCM_IOCTL_STATUS_EXT   0xc0984124
#define SNDRV_PCM_IOCTL_PREPARE      0x00004140
#define SNDRV_PCM_IOCTL_RESET        0x00004141
#define SNDRV_PCM_IOCTL_START        0x00004142
#define SNDRV_PCM_IOCTL_DROP         0x00004143
#define SNDRV_PCM_IOCTL_DRAIN        0x00004144
#define SNDRV_PCM_IOCTL_PAUSE        0x40044145
#define SNDRV_PCM_IOCTL_WRITEI_FRAMES 0x40184150
#define SNDRV_PCM_IOCTL_READI_FRAMES 0x80184151

/* Protocol versions reported by SNDRV_{PCM,CTL}_IOCTL_PVERSION.  alsa-lib
 * compares the major version with its own SNDRV_*_VERSION and refuses the
 * device with SND_ERROR_INCOMPATIBLE_VERSION ("Sound protocol is not
 * compatible") when they differ, so these must follow sound/asound.h. */
#define SNDRV_PCM_VERSION            0x00020012
#define SNDRV_CTL_VERSION            0x00020009

/* ALSA control ioctls; commands and encoded sizes follow sound/asound.h. */
#define SNDRV_CTL_IOCTL_PVERSION     0x80045500
#define SNDRV_CTL_IOCTL_CARD_INFO    0x81785501
#define SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE 0x80045530
#define SNDRV_CTL_IOCTL_PCM_INFO     0xc1205531
#define SNDRV_CTL_IOCTL_PCM_PREFER_SUBDEVICE 0x40045532

/* Create the ALSA PCM node backend (playback=1 or capture=0). */
int alsa_pcm_create_file(int playback);

/* Create the ALSA control node backend. */
int alsa_control_create_file(void);

/* vfile variants used by the /dev/snd devfs open path. */
struct vfile *alsa_pcm_create_vfile(int playback);
struct vfile *alsa_control_create_vfile(void);

#endif /* _DRIVERS_AUDIO_ALSA_H */
