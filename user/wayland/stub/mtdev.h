/* Minimal mtdev stub for A20OS: no MT protocol-B devices exist, so the
 * conversion layer never has work to do.  ABI matches the subset of the
 * real mtdev.h that libinput touches. */
#ifndef _A20_STUB_MTDEV_PLUMBING_H
#define _A20_STUB_MTDEV_PLUMBING_H

#include <linux/input.h>

#ifndef ABS_MT_CNT
#define ABS_MT_CNT (ABS_MT_MAX + 1)
#endif
#ifndef ABS_MT_MAX
#define ABS_MT_MAX 0x3f
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct mtdev_caps {
    struct input_absinfo slot;
    struct input_absinfo abs[ABS_MT_CNT];
    int has_abs[ABS_MT_CNT];
    int has_mt_data;
    int has_slot;
};

struct mtdev {
    int fd;
    struct mtdev_caps caps;
};

struct mtdev_event {
    struct input_event ev;
};

int mtdev_open(struct mtdev *mtdev, int fd);
struct mtdev *mtdev_new_open(int fd);
void mtdev_close(struct mtdev *mtdev);
void mtdev_close_delete(struct mtdev *mtdev);
int mtdev_get(struct mtdev *mtdev, int fd, struct input_event *ev, int num_ev);
int mtdev_get_event(struct mtdev *mtdev, struct input_event *ev);
int mtdev_put_event(struct mtdev *mtdev, const struct input_event *ev);
int mtdev_empty(struct mtdev *mtdev);
int mtdev_idle(struct mtdev *mtdev, int fd, int ms);
void mtdev_set_abs_fuzz(struct mtdev *mtdev, int code, int value);
int mtdev_get_abs_fuzz(struct mtdev *mtdev, int code);

#ifdef __cplusplus
}
#endif

#endif
