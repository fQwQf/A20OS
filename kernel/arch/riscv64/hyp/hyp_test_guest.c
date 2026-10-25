/* ISA-specific executable inputs for the common hardware smoke. */
#include "hyp/hyp_test.h"
static const uint32_t code[] = {
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
    };
    return &p;
}
