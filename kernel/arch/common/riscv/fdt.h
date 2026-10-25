#ifndef A20_RISCV_FIRMWARE_FACTS_H
#define A20_RISCV_FIRMWARE_FACTS_H
#include "firmware/devicetree.h"
#include "boot/context.h"
#include "core/string.h"
#include "fdt_isa.h"
static inline int riscv_firmware_tree(fw_dt_t *tree) {
    const boot_context_t *b = boot_context_get();
    return b && b->fdt && fw_dt_init(tree, b->fdt, b->fdt_available) == 0;
}
static inline int riscv_firmware_extension(const char *extension) {
    fw_dt_t tree;
    fw_node_t root, cpus, cpu;
    if (!extension || !riscv_firmware_tree(&tree) || fw_dt_root(&tree, &root) ||
        fw_dt_find_path(&root, "/cpus", &cpus))
        return 0;
    int rc = cpus.ops->next_child(&cpus, NULL, &cpu);
    while (!rc) {
        fw_property_t p;
        if (cpu.ops->available(&cpu) == 1 &&
            cpu.ops->property(&cpu, "device_type", &p) == 0 && p.size == 4 &&
            !memcmp(p.data, "cpu", 4)) {
            const char *keys[] = {"riscv,isa", "riscv,isa-extensions"};
            for (unsigned i = 0; i < 2; i++)
                if (cpu.ops->property(&cpu, keys[i], &p) == 0 && p.size &&
                    !((const uint8_t *)p.data)[p.size - 1] &&
                    fdt_isa_string_has_extension(p.data, p.size, extension,
                                                 i == 0))
                    return 1;
        }
        fw_node_t next;
        rc = cpus.ops->next_child(&cpus, &cpu, &next);
        if (!rc)
            cpu = next;
    }
    return 0;
}
static inline uint64_t riscv_firmware_timebase(void) {
    fw_dt_t tree;
    fw_node_t root, cpus;
    fw_property_t p;
    if (!riscv_firmware_tree(&tree) || fw_dt_root(&tree, &root) ||
        fw_dt_find_path(&root, "/cpus", &cpus) ||
        cpus.ops->property(&cpus, "timebase-frequency", &p) || p.size != 4)
        return 0;
    return fdt_u32(p.data);
}
#endif
