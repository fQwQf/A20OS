/* A20OS user-mode C runtime startup
 * Reads argc/argv/envp from stack and calls main().
 * The kernel sets up the stack via elf_setup_stack().
 *
 * Stack layout at entry ($sp):
 *   [sp+0]  = argc
 *   [sp+8]  = argv[0] ... argv[argc-1] ... NULL
 *   ...     = envp[0] ... NULL
 *   ...     = auxv[] ... AT_NULL
 */

    .section .text.entry
    .global _start

_start:
    /* Setup Global Pointer (gp) for medany addressing */
.option push
.option norelax
    la gp, __global_pointer$
.option pop

    /* Load argc from stack */
    ld      a0, 0(sp)          /* a0 = argc */
    addi    a1, sp, 8          /* a1 = argv */

    /* Compute envp = argv + argc + 1 */
    slli    t0, a0, 3          /* t0 = argc * 8 */
    add     a2, a1, t0
    addi    a2, a2, 8          /* a2 = envp (skip the NULL terminator) */

    /* Align stack to 16 bytes */
    andi    sp, sp, ~15

    /* Call main(argc, argv, envp) */
    call    main

    /* Exit with main's return value */
    mv      a0, a0             /* exit code in a0 */
    li      a7, 93             /* SYS_exit */
    ecall

    /* Should never reach here */
1:  j       1b
