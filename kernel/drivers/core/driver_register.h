/*
 * A20OS Driver Model — Driver Registration Macros
 *
 * DRIVER_REGISTER() places a pointer to the driver_t in a special
 * linker section (.driver_init).  driver_core_init() iterates this
 * section at boot and calls driver_register() for each entry.
 *
 * For loadable modules (future): DRIVER_MODULE() exports _driver_info
 * which the module loader resolves.
 */
#ifndef _DRIVER_REGISTER_H
#define _DRIVER_REGISTER_H

#include "drivers/core/driver_core.h"

/* ============================================================
 * Built-in driver registration
 *
 * Usage: DRIVER_REGISTER(my_driver);
 * Place in the driver .c file after the driver_t definition.
 * ============================================================ */
#define DRIVER_REGISTER(drv)                                        \
    __attribute__((used, section(".driver_init")))                  \
    static const driver_t * const __drv_##drv = &(drv)

/* ============================================================
 * Module driver registration (future — loadable .kdrv files)
 * ============================================================ */
#define DRIVER_MODULE(drv)                                          \
    driver_t _driver_info __attribute__((used, visibility("default"))) = (drv)

/* ============================================================
 * Board config registration — same pattern as drivers
 * ============================================================ */
#define BOARD_REGISTER(board)                                       \
    __attribute__((used, section(".board_init")))                   \
    static const board_config_t * const __board_##board = &(board)

#endif /* _DRIVER_REGISTER_H */
