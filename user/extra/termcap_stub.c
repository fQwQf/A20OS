/*
 * Minimal termcap implementation with VT100/xterm escape sequences.
 * Provides enough terminal capabilities for vim to render correctly
 * on a serial console that supports standard ANSI escapes.
 * Self-contained: no libc string functions needed for configure test.
 */
static int streq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void smov(void *d, const void *s, unsigned n)
{ char *dd = d; const char *ss = s; while (n--) *dd++ = *ss++; }

static int itostr(char *buf, int n)
{
    if (n < 0) { *buf++ = '-'; n = -n; }
    char tmp[12]; int i = 0;
    if (n == 0) tmp[i++] = '0';
    while (n > 0) { tmp[i++] = '0' + n % 10; n /= 10; }
    char *o = buf;
    while (i > 0) *o++ = tmp[--i];
    *o = '\0';
    return (int)(o - buf);
}

static char termcap_buf[2048];
static char *termcap_ptr;

static const char *tc_lookup(const char *id)
{
    struct tc_entry { const char *id; const char *val; };
    static const struct tc_entry entries[] = {
        {"am",   ""},
        {"cm", "\033[%i%d;%dH"},
        {"cl", "\033[H\033[2J"},
        {"ce", "\033[K"},
        {"cd", "\033[J"},
        {"al", "\033[L"},
        {"dl", "\033[M"},
        {"im", ""},
        {"ei", ""},
        {"ic", ""},
        {"dc", "\033[P"},
        {"dm", ""},
        {"ed", ""},
        {"ti", ""},
        {"te", ""},
        {"ks", ""},
        {"ke", ""},
        {"vi", "\033[?25l"},
        {"ve", "\033[?25h"},
        {"vs", ""},
        {"mb", "\033[5m"},
        {"md", "\033[1m"},
        {"me", "\033[0m"},
        {"mr", "\033[7m"},
        {"mh", "\033[2m"},
        {"mk", "\033[8m"},
        {"us", "\033[4m"},
        {"ue", "\033[0m"},
        {"so", "\033[7m"},
        {"se", "\033[0m"},
        {"up", "\033[A"},
        {"do", "\033[B"},
        {"nd", "\033[C"},
        {"le", "\033[D"},
        {"ho", "\033[H"},
        {"cr", "\r"},
        {"nl", "\n"},
        {"bc", "\010"},
        {"bs", ""},
        {"sf", "\033[S"},
        {"sr", "\033[T"},
        {"RI", "\033[%dC"},
        {"LE", "\033[%dD"},
        {"UP", "\033[%dA"},
        {"DO", "\033[%dB"},
        {"AL", "\033[%dL"},
        {"DL", "\033[%dM"},
        {"IC", "\033[%d@"},
        {"DC", "\033[%dP"},
        {"cs", "\033[%i%d;%dr"},
        {"cl", "\033[H\033[2J"},
        {"ku", "\033[A"},
        {"kd", "\033[B"},
        {"kr", "\033[C"},
        {"kl", "\033[D"},
        {"k1", "\033OP"},
        {"k2", "\033OQ"},
        {"k3", "\033OR"},
        {"k4", "\033OS"},
        {"k5", "\033[15~"},
        {"k6", "\033[17~"},
        {"k7", "\033[18~"},
        {"k8", "\033[19~"},
        {"k9", "\033[20~"},
        {"kb", "\010"},
        {"kh", "\033[H"},
        {"@7", "\033[F"},
        {"kI", "\033[2~"},
        {"kD", "\033[3~"},
        {"kP", "\033[5~"},
        {"kN", "\033[6~"},
        {"r1", ""},
        {"r2", ""},
        {"rs", ""},
        {"is", ""},
        {"it", "\t"},
        {"pt", ""},
        {0, 0}
    };
    for (int i = 0; entries[i].id; i++) {
        if (streq(id, entries[i].id))
            return entries[i].val;
    }
    return 0;
}

int tgetent(char *bp, const char *name)
{
    (void)bp;
    (void)name;
    termcap_ptr = termcap_buf;
    return 1;
}

int tgetnum(const char *id)
{
    if (streq(id, "co")) return 80;
    if (streq(id, "li")) return 24;
    return -1;
}

int tgetflag(const char *id)
{
    const char *val = tc_lookup(id);
    return (val && val[0] == '\0') ? 1 : 0;
}

char *tgetstr(const char *id, char **area)
{
    const char *val = tc_lookup(id);
    if (!val) return 0;
    if (val[0] == '\0') return (char *)val;
    char *p = *area;
    unsigned len = (unsigned)slen(val) + 1;
    smov(p, val, len);
    *area = p + len;
    return p;
}

char *tgoto(const char *cm, int col, int line)
{
    static char buf[64];
    /* Handle %i (%d increments) and simple %d patterns */
    const char *s = cm;
    char *o = buf;
    int args[2] = { line, col };
    int ai = 0;
    while (*s && o < buf + sizeof(buf) - 4) {
        if (*s == '%') {
            s++;
            if (*s == 'i') { args[0]++; args[1]++; s++; continue; }
            if (*s == 'd') {
                o += itostr(o, args[ai]);
                ai++;
                s++;
                continue;
            }
            if (*s == '%') { *o++ = '%'; s++; continue; }
        }
        *o++ = *s++;
    }
    *o = '\0';
    return buf;
}

int tputs(const char *cp, int affcnt, int (*outc)(int))
{
    (void)affcnt;
    if (!cp) return 0;
    while (*cp) outc((unsigned char)*cp++);
    return 0;
}
