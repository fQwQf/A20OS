#ifndef A20_DRIVERS_ADC_ADC_H
#define A20_DRIVERS_ADC_ADC_H

#include "core/types.h"

typedef struct adc_controller adc_controller_t;
struct adc_controller {
    const char *name;
    unsigned    nchannels;
    int (*read)(struct adc_controller *ctrl, unsigned channel, uint16_t *raw);
    void *priv;
};

int adc_controller_add(struct adc_controller *ctrl);
void adc_controller_remove(struct adc_controller *ctrl);
struct adc_controller *adc_controller_get(void);
int adc_read_raw(unsigned channel, uint16_t *raw);
/* Hi3861 ADC: code = voltage / 4 / 1.8 V * 4096. */
uint32_t adc_raw_to_mv(uint16_t raw);

#endif /* A20_DRIVERS_ADC_ADC_H */
