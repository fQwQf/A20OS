/* Experimental integer-only LVZ hardware smoke payloads, unexecuted locally. */
#include "hyp/hyp_test.h"
static const uint32_t code[] = {
    0x1500002c, 0x1600000c, 0x03848c0d, 0x2980118d, 0x03812004,
    0x0380040b, 0x002b8000, 0x03816404, 0x002b8000, 0x03814004,
    0x002b8000, 0x03802804, 0x002b8000, 0x0380200b, 0x002b8000,
};
static const uint32_t spin[] = {
    0x50000000,
};
static const uint32_t smp_code[] = {
    0x0380040c, 0x298000ac, 0x02bfec0c, 0x0014b0ad, 0x288021ac,
    0x43fffd9f, 0x0380240b, 0x002b8000, 0x50000000,
};
const hyp_test_payload_t *hyp_arch_test_payload(void)
{
    static const hyp_test_payload_t p = {
        .code = code, .code_size = sizeof(code),
        .spin = spin, .spin_size = sizeof(spin),
        .smp_code = smp_code, .smp_size = sizeof(smp_code),
        .check_offset = 0x1004, .check_value = 0x123,
    };
    return &p;
}
