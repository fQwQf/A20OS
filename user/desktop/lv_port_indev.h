
/**
 * @file lv_port_indev_templ.h
 *
 */

/*Copy this file as "lv_port_indev.h" and set this value to "1" to enable content*/
#if 1

#ifndef LV_PORT_INDEV_TEMPL_H
#define LV_PORT_INDEV_TEMPL_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#if defined(LV_LVGL_H_INCLUDE_SIMPLE)
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 * GLOBAL PROTOTYPES
 **********************/
lv_indev_t * lv_port_indev_init(void);

typedef void (*lv_port_key_handler_t)(uint32_t key, void * user_data);

enum {
    LV_PORT_KEY_UP = 0x100,
    LV_PORT_KEY_DOWN,
    LV_PORT_KEY_RIGHT,
    LV_PORT_KEY_LEFT,
    LV_PORT_KEY_DELETE,
    LV_PORT_KEY_HOME,
    LV_PORT_KEY_END,
    LV_PORT_KEY_PAGE_UP,
    LV_PORT_KEY_PAGE_DOWN,
};

lv_indev_t * lv_port_keyboard_init(void);
void lv_port_indev_set_key_handler(lv_port_key_handler_t handler, void * user_data);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LV_PORT_INDEV_TEMPL_H*/

#endif /*Disable/Enable content*/
