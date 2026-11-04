#ifndef A20_DRIVERS_GPIO_GPIO_H
#define A20_DRIVERS_GPIO_GPIO_H

#include "core/types.h"

/*
 * GPIO/pinctrl controller API.
 *
 * A controller owns a contiguous GPIO number range and the pin-mux/pull
 * controls for those pins.  Drivers bind to a platform device and register a
 * gpio_chip_t; consumers use the global number.  Registration is a boot-time
 * operation, so the small registry needs no lock on the single-core MCU.
 */
#define GPIO_DIR_IN   0
#define GPIO_DIR_OUT  1

#define GPIO_PULL_NONE 0
#define GPIO_PULL_UP   1
#define GPIO_PULL_DOWN 2

typedef struct gpio_chip gpio_chip_t;
struct gpio_chip {
    const char *name;
    int         base;      /* first global GPIO number */
    unsigned    ngpio;
    int (*set_direction)(struct gpio_chip *chip, unsigned offset, int out);
    int (*get_direction)(struct gpio_chip *chip, unsigned offset);
    int (*set_value)(struct gpio_chip *chip, unsigned offset, int value);
    int (*get_value)(struct gpio_chip *chip, unsigned offset);
    int (*set_pull)(struct gpio_chip *chip, unsigned offset, unsigned pull);
    int (*set_function)(struct gpio_chip *chip, unsigned offset, unsigned func);
    int (*set_input_enable)(struct gpio_chip *chip, unsigned offset, int enable);
    void *priv;
};

int gpio_chip_add(struct gpio_chip *chip);
void gpio_chip_remove(struct gpio_chip *chip);
struct gpio_chip *gpio_chip_get(unsigned gpio);

int gpio_set_direction(unsigned gpio, int out);
int gpio_get_direction(unsigned gpio);
int gpio_set_value(unsigned gpio, int value);
int gpio_get_value(unsigned gpio);
int gpio_set_pull(unsigned gpio, unsigned pull);
int gpio_set_function(unsigned gpio, unsigned func);
int gpio_set_input_enable(unsigned gpio, int enable);

#endif /* A20_DRIVERS_GPIO_GPIO_H */
