# RV32IMC machine-mode execution environment, selected by a board's board.mk.
ifneq ($(PROFILE),mcu)
$(error RV32 M-mode boards require PROFILE=mcu)
endif
ifneq ($(NR_CPUS),1)
$(error RV32 M-mode MCU firmware requires NR_CPUS=1)
endif
ifneq ($(ABI),native)
$(error RV32 M-mode MCU firmware requires ABI=native)
endif
ARCH_CFLAGS := -march=rv32imc_zicsr_zifencei -mabi=ilp32 -mcmodel=medany \
               -ffunction-sections -fdata-sections -mno-relax -msmall-data-limit=0 -fno-pic -fno-stack-protector \
               -DCONFIG_RISCV_MMODE -DCONFIG_UP_SOFT_ATOMICS -DCONFIG_MCU_KERNEL_STACK_SIZE=4096 -DCONFIG_MCU_HEAP_ALIGNMENT=16 \
               -DCONFIG_MCU_DISABLE_CLUSTER_LEAF -DCONFIG_WAIT_TIMER_HEAP_MAX=32
ARCH_LDFLAGS += -Wl,--gc-sections
CROSS_PREFIX := $(RISCV_ELF_PREFIX)
# Only link a runtime that actually exists.  clang's -print-libgcc-file-name
# prints a compiler-rt path even when the target's builtins archive was not
# installed; the kernel already carries compiler-builtins through Rust.
ARCH_LIBS := $(shell f=$$($(CROSS_PREFIX)gcc $(ARCH_CFLAGS) -print-libgcc-file-name 2>/dev/null); test -f "$$f" && echo "$$f")
ARCH_KERNEL_C_SRC := $(wildcard kernel/arch/riscv32/mcu/*.c)
ARCH_KERNEL_ASM_SRC := $(wildcard kernel/arch/riscv32/mcu/*.S)
