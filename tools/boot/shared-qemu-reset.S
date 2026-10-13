/* A20OS secondary reset ROM for an unmodified QEMU PC, not a QEMU patch.
 * No shared RAM is written before the primary kernel publishes its AP entry.
 */
.section .text
.code16
.global _start
_start:
    cli
    cld
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movb $0xff, %al
    outb %al, $0x21
    outb %al, $0xa1
    inb $0x92, %al
    orb $2, %al
    outb %al, $0x92
1:
    pause
    cmpl $0xa20c0001, 0x6f00
    jne 1b
    ljmp $0, $0x7000
.org 0xfff0
    ljmp $0xf000, $0
.org 0x10000
