#ifndef A20_GPIO_FIRMWARE_INPUT_H
#define A20_GPIO_FIRMWARE_INPUT_H
#include "firmware/node.h"

/* Borrow a firmware-configured input; never change pin direction or IRQ state.
 * The mapping has boot lifetime, like its immutable firmware provider. This
 * adapter is resident infrastructure, not an unloadable GPIO controller. */
typedef struct gpio_firmware_input {
    const volatile uint32_t *reg;
    uint32_t mask;
    uint8_t active_low;
} gpio_firmware_input_t;

int gpio_firmware_input_get(const fw_node_t *consumer, const char *property,
                            gpio_firmware_input_t *out);
int gpio_firmware_input_pending(const gpio_firmware_input_t *input);
#endif
