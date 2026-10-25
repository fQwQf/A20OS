/* ISA-specific executable inputs for the common hardware smoke. */
#include "hyp/hyp_test.h"
static const uint32_t spin[] = { 0x0000006f };
static const uint32_t smp_code[] = {
    /* ready[hart] = 1; wait for release; SBI hart_stop. */
    0x00100293, 0x0055a023, 0xffb5f313, 0x00832283, 0xfe028ee3,
    0x004858b7, 0x34d88893, 0x00100813, 0x00000073, 0x0000006f
};
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
    static const hyp_test_payload_t p = {
        .code = code, .code_size = sizeof(code),
        .smp_code = smp_code, .smp_size = sizeof(smp_code),
        .spin = spin, .spin_size = sizeof(spin),
        .check_offset = 0x1004, .check_value = 0x123,
    };
    return &p;
}
