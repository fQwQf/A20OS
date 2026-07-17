/*
 * Smart home hub — infrared remote receiver (NEC) on PB9 / EXTI9.
 *
 * Ported from docs/pz/28-红外遥控实验. The whole NEC frame is decoded
 * synchronously inside the EXTI9_5 interrupt by measuring high-pulse widths.
 *
 * Hardware only: QEMU's stm32vldiscovery has no IR signal source, so init is a
 * no-op there and no interrupt is enabled. NOT yet verified on real hardware.
 * (A production version should use a timer-capture decoder rather than a
 * busy-loop in the ISR — see docs/stm32-big-exp.md §5.3.)
 */
#ifndef _STM32F103_IR_H
#define _STM32F103_IR_H

#include "core/types.h"
#include "fs/fat32lite.h"

#define IR_MAP_MAX 16U

typedef enum ir_action {
    IR_ACT_NONE = 0,
    IR_ACT_FAN_UP,
    IR_ACT_FAN_DOWN,
    IR_ACT_PUMP_TOGGLE,
    IR_ACT_THEME_CYCLE,
    IR_ACT_TALK,
    IR_ACT_MUTE_BUZZER,
    IR_ACT_MENU,
    IR_ACT_LIGHT_UP,
    IR_ACT_LIGHT_DOWN,
} ir_action_t;

/*
 * Built-in bindings for the bundled NEC remote's digit keys 1..9, so the
 * remote works on a card with no /CFG/IR.CFG. A parsed /CFG/IR.CFG replaces
 * the whole table (ir_map_parse resets it), which is how a different remote
 * gets adopted without a rebuild.
 */
#define IR_DEFAULT_MAP                                                        \
    "0x00FF30CF FAN_UP\n"      /* 1 */                                        \
    "0x00FF18E7 FAN_DOWN\n"    /* 2 */                                        \
    "0x00FF7A85 PUMP_TOGGLE\n" /* 3 */                                        \
    "0x00FF10EF THEME_CYCLE\n" /* 4 */                                        \
    "0x00FF38C7 TALK\n"        /* 5 */                                        \
    "0x00FF5AA5 MUTE_BUZZER\n" /* 6 */                                        \
    "0x00FF42BD MENU\n"        /* 7 */                                        \
    "0x00FF4AB5 LIGHT_UP\n"    /* 8 */                                        \
    "0x00FF52AD LIGHT_DOWN\n"  /* 9 */

typedef struct ir_map_binding {
    uint32_t code;
    ir_action_t action;
} ir_map_binding_t;

/* Configure PB9 + EXTI9 + NVIC. 0 ok, -1 skipped (QEMU). */
int stm32_ir_init(void);

/* If a new 32-bit code has been decoded since the last poll, store it in
 * *code and return 1; otherwise return 0. */
int stm32_ir_poll(uint32_t *code);

/* EXTI9_5 interrupt body — called from the arch IRQ stub via trap.c. */
void stm32_ir_isr(void);

/* Data-driven NEC-code bindings loaded from /CFG/IR.CFG. */
int ir_map_load_default(void);
int ir_map_load_fs(fat32lite_fs_t *fs, const char *path);
int ir_map_dispatch(uint32_t code);
unsigned ir_map_count(void);

/* Action id -> name, for the bring-up log ("unbound" for IR_ACT_NONE). */
const char *ir_action_name(int action);

/* Pure parser: reset `table`, parse up to `max` bindings, return their count. */
int ir_map_parse(const char *text, unsigned len, ir_map_binding_t *table,
                 unsigned max);

#endif /* _STM32F103_IR_H */
