/* ISA-specific executable inputs for the common hardware smoke. */
#include "hyp/hyp_test.h"
static const uint32_t spin[] = { 0x0000006f };
extern const uint8_t hyp_sbi_smp_start[], hyp_sbi_smp_end[];
extern int hyp_arch_sbi_selftest(void);
static const uint32_t code[] = {
    0x800012b7, 0x12300313, 0x0062a223,
    0x04800513, 0x00100893, 0x00000073,
    0x05900513, 0x00100893, 0x00000073,
    0x05000513, 0x00100893, 0x00000073,
    0x00a00513, 0x00100893, 0x00000073,
    0x00800893, 0x00000073,
};

const hyp_test_payload_t *hyp_arch_test_payload(void)
{
    static hyp_test_payload_t p = {
        .code = code, .code_size = sizeof(code),
        .smp_check_offset = 0x100c, .smp_check_value = 0x51b1,
        .abi_selftest = hyp_arch_sbi_selftest,
        .spin = spin, .spin_size = sizeof(spin),
        .check_offset = 0x1004, .check_value = 0x123,
    };
    /* Used serially by opt-in boot diagnostics before workers are published. */
    p.smp_code = hyp_sbi_smp_start;
    p.smp_size = (size_t)(hyp_sbi_smp_end - hyp_sbi_smp_start);
    return &p;
}
