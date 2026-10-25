/* ISA-specific executable inputs for the common hardware smoke. */
#include "hyp/hyp_test.h"
#include "hyp/hyp_backend.h"
static uint8_t code[] = {
    0xb8,1,0,0,0, 0xbb,'H',0,0,0, 0x0f,0x01,0xd9,
    0xb8,1,0,0,0, 0xbb,'Y',0,0,0, 0x0f,0x01,0xd9,
    0xb8,1,0,0,0, 0xbb,'P',0,0,0, 0x0f,0x01,0xd9,
    0xb8,1,0,0,0, 0xbb,'\n',0,0,0, 0x0f,0x01,0xd9,
    0xc7,0x05,0x04,0x10,0x10,0,0x5a,0,0,0, /* [0x101004]=0x5a */
    0xb8,8,0,0,0, 0x0f,0x01,0xd9,
};
static uint8_t smp[] = {
    0xc7,0x06,1,0,0,0,      /* mov dword [esi],1 */
    0x89,0xf2,0x83,0xe2,0xfb, /* edx=esi&~4 */
    0x8b,0x42,8,0x85,0xc0,0x74,0xf9, /* wait [edx+8]!=0 */
    0xb8,10,0,0,0,0x0f,0x01,0xd9, /* CPU_OFF */
};
static const uint8_t spin[] = {0xfa, 0xeb, 0xfe}; /* cli; jmp . */
const hyp_test_payload_t *hyp_arch_test_payload(void)
{
    /* Select the architecturally valid call instruction at runtime. */
    const hyp_backend_t *b = hyp_arch_backend();
    if (b && b->name[4] == 'v') {
        smp[sizeof(smp)-1] = 0xc1;
        for (unsigned i = 2; i < sizeof(code); i++)
            if (code[i-2] == 0x0f && code[i-1] == 0x01 && code[i] == 0xd9) code[i] = 0xc1;
    }
    static const hyp_test_payload_t p = {
        .code = code, .code_size = sizeof(code),
        .spin = spin, .spin_size = sizeof(spin),
        .smp_code = smp, .smp_size = sizeof(smp),
        .check_offset = 4096 + 4, .check_value = 0x5a,
    };
    return &p;
}
