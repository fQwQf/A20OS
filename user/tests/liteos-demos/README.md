# LiteOS textbook experiments on A20OS

The five experiments from the LiteOS/OpenHarmony course and real-time validation,
ported to the A20OS CMSIS-RTOS2 personality. One static-PIE image contains all
five; the kernel loads it into an owned flat space and runs it in U-mode.

| Experiment | Source | API surface |
|---|---|---|
| 4.1 threads | `demo.c` | `cmsis_os2.h` only |
| 4.2 semaphore | `demo.c` | `cmsis_os2.h` only |
| 4.3 software timers | `demo.c` | `cmsis_os2.h` only |
| 4.4 interrupts | `interrupt.c` | `cmsis_os2.h` + `hi_isr.h` / `iot_gpio.h` / `iot_pwm.h` |
| 4.5 real-time | `rt.c` | `cmsis_os2.h` only (creation priority, SetPriority, osKernelLock, mutex PI, chain PI, initial thread PI) |

`runtime.c` is a freestanding runtime (log ring, 64-bit division, assertions)
and `start.c` is the ELF entry point.  The run record is placed on the user
stack by the loader and passed in `a0`; that is how the experiments report
results without a non-CMSIS console API.

## Layering

```
user/cmsis_rtos2/          standard CMSIS-RTOS2; never includes hi_*/IoT*
user/port/hi3861_hal/      vendor extension: hi_irq_*, IoTGpio*, IoTPwm*
kernel/arch/riscv32/mcu/vendor_hi.c
                           vendor syscall range 0x7F00.., outside the
                           generated Native table
```

The vendor IRQ model is a deferred one: the kernel increments and wakes a
user futex word, and a CMSIS dispatcher thread runs the handler.  A user
program on this NOMMU part cannot execute an ISR, so deferral is the only
correct shape.  On QEMU `hi_irq_trigger()` produces the edge; on Hi3861 the
same raise path can be driven by the real pin interrupt.

## Run

```sh
# Host behaviour suite (POSIX + Native backends)
make -C user/cmsis_rtos2 test

# Kernel + demo in QEMU
tools/a20 test qemu-riscv32-mcu
```

Expected tail:

```text
[4.1] PASS steps=5/5
[4.2] PASS taken=5 produced=5
[4.3] PASS once=1 periodic=12
[4.4] PASS irq=5
[4.5] PASS create=1 setprio=1 lock=1 inherit=1 chain=1 init=1
[DEMO] experiments 4.1-4.9: PASS
```

The QEMU board enables `CONFIG_MCU_USER_DEMO` in
`kernel/arch/riscv32/boards/qemu-virt-riscv32-mcu/board.mk`.  Hi3861 builds
the same flat-program infrastructure; the demo is opt-in until U-mode is
confirmed on the physical part:

```sh
tools/a20 package hi3861 -- HI3861_LITEOS_DEMO=1
tools/a20 flash   hi3861 -- HI3861_LITEOS_DEMO=1
```

The flash tool writes only kernel A (0xD000); rebuilding without the flag
restores the plain shell image.
