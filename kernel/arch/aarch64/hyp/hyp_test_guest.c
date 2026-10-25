/* ISA-specific executable inputs for the common hardware smoke. */
#include "hyp/hyp_test.h"
static const uint32_t code[] = {
    0xd2800020, /* mov x0, #1 */
    0xd2800901, /* mov x1, #'H' */
    0xd4000002, /* hvc #0 */
    0xd2800020, 0xd2800b21, 0xd4000002, /* Y */
    0xd2800020, 0xd2800a01, 0xd4000002, /* P */
    0xd2800020, 0xd2800141, 0xd4000002, /* newline */
    0xd2820082, 0xf2a80002, /* x2 = 0x40001004, demand-filled page */
    0x52800b43, 0xb9000043, /* w3=0x5a; str w3,[x2] */
    0xd2800100, 0xd4000002, /* mov x0,#8; hvc #0 */
};
static const uint32_t spin[] = {0x14000000}; /* b . */
static const uint32_t smp_code[] = {
    0x52800021, 0xb9000001, 0x92800082, 0x8a020002, 0xb9400841, 0x34ffffe1, 0xd2800040, 0xf2b08000, 0xd4000002, 0x14000000
};
const hyp_test_payload_t *hyp_arch_test_payload(void)
{
    static const hyp_test_payload_t p = {
        .code = code, .code_size = sizeof(code),
        .spin = spin, .spin_size = sizeof(spin),
        .check_offset = 4096 + 4, .check_value = 0x5a,
        .smp_code = smp_code, .smp_size = sizeof(smp_code),
    };
    return &p;
}
