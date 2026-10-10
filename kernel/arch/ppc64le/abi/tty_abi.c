/* Linux PowerPC ioctl numbers and termios wire layout. */
#include "core/ioctl.h"
#include "core/string.h"
#include "fs/tty.h"
#include "sys/usercopy.h"

#define PPC64_TCGETS 0x402C7413UL
#define PPC64_TCSETS 0x802C7414UL
#define PPC64_TCSETSW 0x802C7415UL
#define PPC64_TCSETSF 0x802C7416UL
#define PPC64_TIOCSPGRP 0x80047476UL
#define PPC64_TIOCGPGRP 0x40047477UL
#define PPC64_TIOCSWINSZ 0x80087467UL
#define PPC64_TIOCGWINSZ 0x40087468UL
#define PPC64_FIONBIO 0x8004667EUL
#define PPC64_FIONREAD 0x4004667FUL

typedef struct {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t c_cc[KTTY_NCCS];
    uint8_t c_line;
    uint32_t c_ispeed;
    uint32_t c_ospeed;
} ppc64_tty_termios_t;

_Static_assert(sizeof(ppc64_tty_termios_t) == 44,
               "PPC64 termios must be 44 bytes");

unsigned long arch_linux_ioctl_request(unsigned long req) {
    switch (req) {
        case PPC64_TCGETS:
            return TCGETS;
        case PPC64_TCSETS:
            return TCSETS;
        case PPC64_TCSETSW:
            return TCSETSW;
        case PPC64_TCSETSF:
            return TCSETSF;
        case PPC64_TIOCSPGRP:
            return TIOCSPGRP;
        case PPC64_TIOCGPGRP:
            return TIOCGPGRP;
        case PPC64_TIOCSWINSZ:
            return TIOCSWINSZ;
        case PPC64_TIOCGWINSZ:
            return TIOCGWINSZ;
        case PPC64_FIONBIO:
            return FIONBIO;
        case PPC64_FIONREAD:
            return FIONREAD;
        default:
            return req;
    }
}

long arch_tty_termios_to_user(void *dst, const ktty_termios_t *src) {
    ppc64_tty_termios_t wire = {
        .c_iflag = src->c_iflag,
        .c_oflag = src->c_oflag,
        .c_cflag = src->c_cflag,
        .c_lflag = src->c_lflag,
        .c_line = src->c_line,
    };
    memcpy(wire.c_cc, src->c_cc, KTTY_NCCS);
    return copy_to_user(dst, &wire, sizeof(wire));
}

long arch_tty_termios_from_user(ktty_termios_t *dst, const void *src) {
    ppc64_tty_termios_t wire;
    long r = copy_from_user(&wire, src, sizeof(wire));
    if (r < 0)
        return r;
    dst->c_iflag = wire.c_iflag;
    dst->c_oflag = wire.c_oflag;
    dst->c_cflag = wire.c_cflag;
    dst->c_lflag = wire.c_lflag;
    dst->c_line = wire.c_line;
    memcpy(dst->c_cc, wire.c_cc, KTTY_NCCS);
    return r;
}
