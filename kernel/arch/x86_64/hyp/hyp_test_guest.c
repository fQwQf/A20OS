/* ISA-specific executable inputs for the common hardware smoke. */
#include "hyp/hyp_test.h"
static const uint8_t code[] = {
    0xb8,1,0,0,0, 0xbb,'H',0,0,0, 0x0f,0x01,0xd9,
    0xb8,1,0,0,0, 0xbb,'Y',0,0,0, 0x0f,0x01,0xd9,
    0xb8,1,0,0,0, 0xbb,'P',0,0,0, 0x0f,0x01,0xd9,
    0xb8,1,0,0,0, 0xbb,'\n',0,0,0, 0x0f,0x01,0xd9,
    0xc7,0x05,0x04,0x10,0x10,0,0x5a,0,0,0, /* [0x101004]=0x5a */
    0xb8,8,0,0,0, 0x0f,0x01,0xd9,
};
static const uint8_t spin[] = {0xfa, 0xeb, 0xfe}; /* cli; jmp . */
const hyp_test_payload_t *hyp_arch_test_payload(void)
{
    static const hyp_test_payload_t p = {
        .code = code, .code_size = sizeof(code),
        .spin = spin, .spin_size = sizeof(spin),
        .check_offset = 4096 + 4, .check_value = 0x5a,
    };
    return &p;
}
