#if 1

#include "lv_port_indev.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include "lvgl.h"

struct input_event {
    unsigned long time_sec;
    unsigned long time_usec;
    unsigned short type;
    unsigned short code;
    unsigned int value;
};

#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03

#define REL_X 0x00
#define REL_Y 0x01

#define BTN_MOUSE    0x110
#define BTN_LEFT     0x110
#define BTN_RIGHT    0x111

static int ev_fd = -1;
static int32_t mouse_x = 0;
static int32_t mouse_y = 0;
static bool mouse_pressed = false;

/* Screen resolution (hardcoded for now, or you can extern fb_width) */
#define MOUSE_MAX_X 1024
#define MOUSE_MAX_Y 768

static void mouse_init(void)
{
    ev_fd = open("/dev/event0", O_RDONLY); // A20OS devfs uses /dev/event0
    if (ev_fd < 0) {
        printf("Failed to open /dev/event0\n");
    } else {
        printf("Opened /dev/event0 for input\n");
    }
}

static void mouse_read(lv_indev_t * indev_drv, lv_indev_data_t * data)
{
    if (ev_fd < 0) {
        data->point.x = mouse_x;
        data->point.y = mouse_y;
        data->state = mouse_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        return;
    }

    struct input_event ev;
    /* Read events in non-blocking mode or process all available.
       A20OS input_read returns copied bytes. We should read until EAGAIN (if implemented).
       Actually, our rewritten input_read blocks if no data.
       Wait! We can't block here or LVGL will hang! We need to make the FD non-blocking. */
    // Wait, let's just set the fd to non-blocking:
    int flags = fcntl(ev_fd, F_GETFL, 0);
    fcntl(ev_fd, F_SETFL, flags | O_NONBLOCK);

    while (read(ev_fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (ev.type == EV_REL) {
            if (ev.code == REL_X) {
                mouse_x += (int32_t)ev.value;
            } else if (ev.code == REL_Y) {
                mouse_y += (int32_t)ev.value;
            }
        } else if (ev.type == EV_KEY) {
            if (ev.code == BTN_LEFT) {
                mouse_pressed = (ev.value != 0);
            }
        }
    }

    /* Clamp mouse position */
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x >= MOUSE_MAX_X) mouse_x = MOUSE_MAX_X - 1;
    if (mouse_y >= MOUSE_MAX_Y) mouse_y = MOUSE_MAX_Y - 1;

    data->point.x = mouse_x;
    data->point.y = mouse_y;
    data->state = mouse_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

lv_indev_t * mouse_indev;

void lv_port_indev_init(void)
{
    mouse_init();

    lv_indev_t * indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, mouse_read);
    
    mouse_indev = indev;
}

#endif
