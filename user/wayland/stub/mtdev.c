/* Minimal mtdev stub for A20OS: no MT protocol-B devices exist. */

#include <mtdev-plumbing.h>
#include <stdlib.h>

int mtdev_open(struct mtdev *mtdev, int fd) { (void)mtdev; (void)fd; return -1; }
struct mtdev *mtdev_new_open(int fd) {
    struct mtdev *m = calloc(1, sizeof(*m));
    if (m)
        m->fd = fd;
    return m;
}
void mtdev_close(struct mtdev *mtdev) { (void)mtdev; }
void mtdev_close_delete(struct mtdev *mtdev) { free(mtdev); }
int mtdev_get(struct mtdev *mtdev, int fd, struct input_event *ev, int num_ev) { (void)mtdev; (void)fd; (void)ev; (void)num_ev; return 0; }
int mtdev_get_event(struct mtdev *mtdev, struct input_event *ev) { (void)mtdev; (void)ev; return 0; }
int mtdev_put_event(struct mtdev *mtdev, const struct input_event *ev) { (void)mtdev; (void)ev; return 0; }
int mtdev_empty(struct mtdev *mtdev) { (void)mtdev; return 1; }
int mtdev_idle(struct mtdev *mtdev, int fd, int ms) { (void)mtdev; (void)fd; (void)ms; return 0; }
void mtdev_set_abs_fuzz(struct mtdev *mtdev, int code, int value) { (void)mtdev; (void)code; (void)value; }
int mtdev_get_abs_fuzz(struct mtdev *mtdev, int code) { (void)mtdev; (void)code; return 0; }
