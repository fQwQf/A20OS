#ifndef _DRIVERS_BUS_PCI_BUS_H
#define _DRIVERS_BUS_PCI_BUS_H

#include "drivers/core/driver_core.h"

extern bus_type_t pci_bus;

bus_type_t *get_pci_bus(void);
int pci_enable_and_assign_bars(device_t *dev);

#endif
