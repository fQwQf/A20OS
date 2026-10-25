#ifndef A20_RISCV64_FDT_ISA_H
#define A20_RISCV64_FDT_ISA_H

/* Pure parser shared by the DTB reader and the host-side regression test. */
static inline int fdt_isa_string_has_extension(const char *value,
                                               unsigned int len,
                                               const char *extension,
                                               int legacy_isa) {
    if (!value || !extension)
        return 0;

    unsigned int scan_len = len;
    if (legacy_isa) {
        for (unsigned int i = 0; i < scan_len; i++) {
            if (!value[i]) {
                scan_len = i;
                break;
            }
        }
    }

    unsigned int ext_len = 0;
    while (extension[ext_len])
        ext_len++;
    if (!ext_len || ext_len > scan_len)
        return 0;

    /* Legacy riscv,isa starts with rv32/rv64 and encodes single-letter
     * extensions in one compact run (e.g. rv64imafdch).  Match only this
     * base run, never a letter inside a multi-letter extension. */
    if (legacy_isa && ext_len == 1 && scan_len >= 4 && value[0] == 'r' &&
        value[1] == 'v' &&
        ((value[2] == '3' && value[3] == '2') ||
         (value[2] == '6' && value[3] == '4'))) {
        for (unsigned int i = 4; i < scan_len && value[i] != '_'; i++)
            if (value[i] == extension[0])
                return 1;
    }

    for (unsigned int off = 0; off + ext_len <= scan_len; off++) {
        int left_ok =
            off == 0 || value[off - 1] == '_' || value[off - 1] == '\0';
        unsigned int after = off + ext_len;
        int right_ok =
            after == scan_len || value[after] == '_' || value[after] == '\0';
        unsigned int i = 0;
        while (i < ext_len && value[off + i] == extension[i])
            i++;
        if (left_ok && right_ok && i == ext_len)
            return 1;
    }
    return 0;
}

#endif
