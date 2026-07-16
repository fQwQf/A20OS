#ifndef A20OS_DESKTOP_TERMINAL_H
#define A20OS_DESKTOP_TERMINAL_H

#include "lvgl.h"

lv_obj_t * desktop_terminal_create(lv_obj_t * parent);
void desktop_terminal_send_command(const char * command);
void desktop_terminal_clear(void);
void desktop_terminal_restart(void);
void desktop_terminal_release_focus(void);
const char * desktop_terminal_get_pty_name(void);
bool desktop_terminal_is_running(void);
int desktop_terminal_get_columns(void);
int desktop_terminal_get_rows(void);

#endif
