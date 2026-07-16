#ifndef A20OS_DESKTOP_APPS_H
#define A20OS_DESKTOP_APPS_H

#include "lvgl.h"

typedef struct desktop_app {
    const char *name;
    const char *symbol;
    const char *desc;
    uint32_t accent;
    lv_obj_t *(*create)(lv_obj_t *parent);
} desktop_app_t;

extern desktop_app_t desktop_app_dashboard;
extern desktop_app_t desktop_app_terminal;
extern desktop_app_t desktop_app_monitor;
extern desktop_app_t desktop_app_processes;
extern desktop_app_t desktop_app_files;
extern desktop_app_t desktop_app_network;
extern desktop_app_t desktop_app_system;

#endif
