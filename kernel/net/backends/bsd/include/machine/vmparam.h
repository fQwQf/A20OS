/* Network buffers use A20OS's direct map, never the FreeBSD kernel's VA map. */
#ifndef A20_BSD_MACHINE_VMPARAM_H
#define A20_BSD_MACHINE_VMPARAM_H
#include_next <machine/vmparam.h>
#undef PHYS_TO_DMAP
#undef DMAP_TO_PHYS
uintptr_t port_phys_to_dmap(uint64_t address);
uint64_t port_dmap_to_phys(uintptr_t address);
#define PHYS_TO_DMAP(pa) port_phys_to_dmap(pa)
#define DMAP_TO_PHYS(va) port_dmap_to_phys(va)
#endif
