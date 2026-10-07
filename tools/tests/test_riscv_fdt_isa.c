#include <assert.h>
#include <string.h>

#include "../../kernel/arch/riscv64/platform/fdt_isa.h"

int main(void) {
    static const char legacy[] = "rv64imafdch_zicntr_zihintpause";
    static const char legacy32[] = "rv32imafch_zicsr";
    static const char extension_list[] = "i\0m\0a\0h\0zihintpause\0";
    static const char no_h[] = "rv64imafd_zicsr";
    static const char misleading[] = "rv64imafd_zihintpause_shgatpa";
    static const char nul_payload[] = "rv64imafd\0h";

    assert(fdt_isa_string_has_extension(legacy, sizeof(legacy) - 1, "h", 1));
    assert(
        fdt_isa_string_has_extension(legacy32, sizeof(legacy32) - 1, "h", 1));
    assert(
        fdt_isa_string_has_extension(legacy, sizeof(legacy) - 1, "zicntr", 1));
    assert(fdt_isa_string_has_extension(extension_list,
                                        sizeof(extension_list) - 1, "h", 0));
    assert(fdt_isa_string_has_extension(
        extension_list, sizeof(extension_list) - 1, "zihintpause", 0));
    assert(!fdt_isa_string_has_extension(no_h, sizeof(no_h) - 1, "h", 1));
    assert(!fdt_isa_string_has_extension(misleading, sizeof(misleading) - 1,
                                         "h", 1));
    assert(!fdt_isa_string_has_extension(nul_payload, sizeof(nul_payload) - 1,
                                         "h", 1));
    assert(!fdt_isa_string_has_extension("zihintpause", 11, "h", 0));
    assert(!fdt_isa_string_has_extension(NULL, 0, "h", 0));
    assert(!fdt_isa_string_has_extension("rv64i", 5, "", 1));
    return 0;
}
