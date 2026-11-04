#ifndef A20_DRIVERS_PWM_PWM_H
#define A20_DRIVERS_PWM_PWM_H

#include "core/types.h"

typedef struct pwm_chip pwm_chip_t;
struct pwm_chip {
    const char *name;
    int         base;
    unsigned    npwm;
    /* duty and period are 16-bit counter values; output duty is duty/period
     * and frequency is the channel clock / period. */
    int (*start)(struct pwm_chip *chip, unsigned channel, uint16_t duty,
                 uint16_t period);
    int (*stop)(struct pwm_chip *chip, unsigned channel);
    void *priv;
};

int pwm_chip_add(struct pwm_chip *chip);
void pwm_chip_remove(struct pwm_chip *chip);
struct pwm_chip *pwm_chip_get(unsigned channel);
int pwm_start(unsigned channel, uint16_t duty, uint16_t period);
int pwm_stop(unsigned channel);

#endif /* A20_DRIVERS_PWM_PWM_H */
