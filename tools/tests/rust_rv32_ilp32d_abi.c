#include <stdint.h>

_Static_assert(sizeof(void *) == 4, "RV32 pointer width must be 32 bits");
_Static_assert(sizeof(uintptr_t) == 4, "RV32 uintptr_t must be 32 bits");
_Static_assert(sizeof(double) == 8, "ILP32D double width must be 64 bits");

extern double a20_rv32_ilp32d_probe(double a, uint32_t b);

double a20_rv32_ilp32d_caller(double a, uint32_t b) {
    return a20_rv32_ilp32d_probe(a, b);
}
