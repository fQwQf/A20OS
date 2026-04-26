/* A20OS user-mode syscall wrappers — RISC-V 64 */

    .section .text

    /* Macro: syscall with N args */

    .global syscall0
syscall0:   /* a0=num */
    mv a7, a0
    li a0, 0
    ecall
    ret

    .global syscall1
syscall1:   /* a0=num, a1=arg0 */
    mv a7, a0
    mv a0, a1
    ecall
    ret

    .global syscall2
syscall2:   /* a0=num, a1=arg0, a2=arg1 */
    mv a7, a0
    mv a0, a1
    mv a1, a2
    ecall
    ret

    .global syscall3
syscall3:
    mv a7, a0
    mv a0, a1
    mv a1, a2
    mv a2, a3
    ecall
    ret

    .global syscall4
syscall4:
    mv a7, a0
    mv a0, a1
    mv a1, a2
    mv a2, a3
    mv a3, a4
    ecall
    ret

    .global syscall5
syscall5:
    mv a7, a0
    mv a0, a1
    mv a1, a2
    mv a2, a3
    mv a3, a4
    mv a4, a5
    ecall
    ret

    .global syscall6
syscall6:
    mv a7, a0
    mv a0, a1
    mv a1, a2
    mv a2, a3
    mv a3, a4
    mv a4, a5
    mv a5, a6
    ecall
    ret
