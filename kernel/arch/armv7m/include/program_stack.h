#ifndef A20_ARMV7M_PROGRAM_STACK_H
#define A20_ARMV7M_PROGRAM_STACK_H
/* A PSP task may migrate; exception handlers need a separate CPU-private
 * stack, disjoint from saved task stacks and the bootstrap/idle stack. */
#define ARMV7M_EXCEPTION_STACK_BYTES 2048
#endif
