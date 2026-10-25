#include "hyp/hyp_test.h"
static const uint32_t code[] = {
    0xe3a00001, 0xe3a01048, 0xe1400070,
    0xe3a00001, 0xe3a01059, 0xe1400070,
    0xe3a00001, 0xe3a01050, 0xe1400070,
    0xe3a00001, 0xe3a0100a, 0xe1400070,
    0xe3012004, 0xe3442000, 0xe3a0305a, 0xe5823000,
    0xe3a00008, 0xe1400070,
};
static const uint32_t spin[] = { 0xeafffffe };
/* r0=unique ready GPA; CPU0/1 must stay simultaneously active until release. */
static const uint32_t smp[] = {
    0xe3a01001, /* mov r1,#1 */
    0xe5801000, /* str r1,[r0] */
    0xf57ff05f, /* dmb sy */
    0xe3c01004, /* bic r1,r0,#4 */
    0xe5912008, /* ldr r2,[r1,#8] */
    0xe3520000, /* cmp r2,#0 */
    0x0afffffc, /* beq ldr */
    0xe3000002, 0xe3480400, /* movw r0,#2; movt r0,#0x8400 */
    0xe1400070, /* hvc #0 (PSCI CPU_OFF) */
    0xeafffffe,
};
const hyp_test_payload_t *hyp_arch_test_payload(void)
{
    static const hyp_test_payload_t p = {
        .code = code, .code_size = sizeof(code), .spin = spin, .spin_size = sizeof(spin),
        .check_offset = 4100, .check_value = 0x5a,
        .smp_code = smp, .smp_size = sizeof(smp),
    };
    return &p;
}
