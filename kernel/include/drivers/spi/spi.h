#ifndef A20_DRIVERS_SPI_SPI_H
#define A20_DRIVERS_SPI_SPI_H

#include "core/types.h"

#define SPI_MODE_CPOL 0x1
#define SPI_MODE_CPHA 0x2

typedef struct spi_controller spi_controller_t;
struct spi_controller {
    const char *name;
    int         bus;
    /* Full-duplex byte transfer.  Chip select is driven by the controller's
     * pin mux (SPI0_CSN / SPI1_CSN), not by a GPIO in this layer. */
    int (*transfer)(struct spi_controller *ctrl, const void *tx, void *rx,
                    size_t len, uint32_t hz, uint8_t mode);
    void *priv;
};

int spi_controller_add(struct spi_controller *ctrl);
void spi_controller_remove(struct spi_controller *ctrl);
struct spi_controller *spi_controller_get(int bus);
int spi_transfer(int bus, const void *tx, void *rx, size_t len, uint32_t hz,
                 uint8_t mode);

#endif /* A20_DRIVERS_SPI_SPI_H */
