#ifndef A20_MCU_DEMO_H
#define A20_MCU_DEMO_H
#include <stdint.h>

/*
 * Run record shared by the MCU flat-program launcher and the learner demo.
 *
 * The launcher copies this structure onto the new user stack and passes its
 * address in a0, so the demo and the kernel read the same bytes without a
 * private syscall or a device node.  The ring is single-producer (demo) and
 * single-consumer (kernel), which is all a two-task test harness needs.
 */
#define A20_MCU_DEMO_MAGIC   0x41323044u /* "A20D" */
#define A20_MCU_DEMO_VERSION 1u
#define A20_MCU_DEMO_LINE    96u
#define A20_MCU_DEMO_LINES   48u

typedef struct a20_mcu_demo_record {
    uint32_t magic;
    uint32_t version;
    volatile uint32_t head;   /* next line slot the demo will write */
    volatile uint32_t tail;   /* next line slot the kernel will read */
    volatile uint32_t done;   /* demo finished all experiments */
    volatile int32_t status;  /* 0 = every experiment passed */
    volatile uint32_t checks; /* assertions executed */
    char lines[A20_MCU_DEMO_LINES][A20_MCU_DEMO_LINE];
} a20_mcu_demo_record_t;

#endif /* A20_MCU_DEMO_H */
