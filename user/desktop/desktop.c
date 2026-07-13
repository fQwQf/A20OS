#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include "lvgl.h"
#include "lv_port_disp.h"
#include "lv_port_indev.h"

// Custom flush callback
static void my_disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    /*The most simple case (but also the slowest) to put all pixels to the screen one-by-one
     *`put_px` is just an example, it needs to be implemented by you.*/
    // int32_t x, y;
    // for(y = area->y1; y <= area->y2; y++) {
    //     for(x = area->x1; x <= area->x2; x++) {
    //         put_px(x, y, *px_map);
    //         px_map++;
    //     }
    // }
    
    // For A20OS, we just call flush IOCTL. lv_port_disp.c handles it.
    lv_display_flush_ready(disp);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("Starting A20OS LVGL Desktop...\n");

    /* Initialize LVGL */
    lv_init();

    /* Initialize Display and Input */
    lv_port_disp_init();
    lv_port_indev_init();

    /* Create a cursor for the mouse */
    lv_obj_t * cursor_obj = lv_image_create(lv_screen_active());
    // Create a simple red square as cursor since we don't have image support loaded
    lv_obj_set_size(cursor_obj, 10, 10);
    lv_obj_set_style_bg_color(cursor_obj, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_bg_opa(cursor_obj, LV_OPA_COVER, 0);
    extern lv_indev_t * mouse_indev;
    if (mouse_indev) {
        lv_indev_set_cursor(mouse_indev, cursor_obj);
    }

    /* Create a window/UI */
    lv_obj_t * win = lv_obj_create(lv_screen_active());
    lv_obj_set_size(win, 400, 300);
    lv_obj_center(win);
    lv_obj_set_style_bg_color(win, lv_color_hex(0x222222), 0);

    lv_obj_t * label = lv_label_create(win);
    lv_label_set_text(label, "Welcome to A20OS Desktop!");
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t * btn = lv_button_create(win);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_t * btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "Click Me");

    printf("UI initialized, entering loop...\n");

    /* Handle LVGL tasks */
    while(1) {
        lv_timer_handler();
        usleep(5000);
    }

    return 0;
}
