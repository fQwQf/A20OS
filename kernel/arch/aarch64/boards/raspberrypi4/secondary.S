    .section .text
    .global rpi4_spin_entry
rpi4_spin_entry:
    /* Firmware spin-table entry has no logical-context argument. Read the
     * published context by physical MPIDR before adopting the common map. */
    mrs     x1, mpidr_el1
    and     x1, x1, #0xffffff
    cmp     x1, #4
    b.hs    1f
    adrp    x2, rpi4_spin_context
    add     x2, x2, :lo12:rpi4_spin_context
    add     x2, x2, x1, lsl #4
    ldp     x0, x3, [x2]
    cbz     x0, 1f
    cbz     x3, 1f
    br      x3
1:
    wfe
    b       1b
