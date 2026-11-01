# A20OS CMSIS-RTOS2 personality.
#
#   make test                      host self-test (POSIX backend)
#   make trim PROFILE=mcu-min      emit cmsis_config.h + syscall table
#   make freestanding PROFILE=mcu  compile the trimmed core for riscv32
#   make trim-check ELF=<elf>      verify a kernel against the profile
#
# The host test validates the portable core; the target builds only swap the
# backend and the feature header.

CC       ?= cc
CFLAGS   ?= -O2 -g
CFLAGS   += -std=gnu11 -Wall -Wextra -Werror -pthread
CPPFLAGS += -Iinclude -Isrc

PROFILE  ?= mcu
TRIM_OUT := build/trim-$(PROFILE)
TRIM     := python3 ../../tools/cmsis_trim.py

CORE  := src/os_kernel.c src/os_thread.c src/os_sync.c src/os_timer.c src/os_queue.c
POSIX := src/backend_posix.c
NATIVE := src/backend_native.c src/native_platform_a20.c
TEST  := tests/test_cmsis.c
OUT   := build/test_cmsis
NATIVE_TEST := build/test_cmsis_native

CC_RISCV32 ?= riscv32-linux-gnu-gcc
RV32FLAGS  := -Os -march=rv32imc -mabi=ilp32 -ffreestanding \
              -ffunction-sections -fdata-sections \
              -Iinclude -Isrc -I../liba20rt -I../../kernel/include/uapi

.PHONY: all test test-posix test-native trim trim-check freestanding clean

all: $(OUT)

$(OUT): $(CORE) $(POSIX) $(TEST)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

# Same core and same test, but through the Native backend with its platform
# calls implemented by pthreads/futex on the host.
$(NATIVE_TEST): $(CORE) src/backend_native.c tests/native_platform_host.c $(TEST)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -DOS_NATIVE_TICK_HZ=1000 $^ -o $@

test: test-posix test-native

test-posix: $(OUT)
	./$(OUT)

test-native: $(NATIVE_TEST)
	./$(NATIVE_TEST)

trim:
	$(TRIM) emit --profile $(PROFILE) --out $(TRIM_OUT)

trim-check:
	@test -n "$(ELF)" || { echo "usage: make trim-check ELF=<kernel.elf> [PROFILE=mcu]" >&2; exit 1; }
	$(TRIM) check --profile $(PROFILE) --elf $(ELF)

freestanding: trim
	@mkdir -p build/freestanding-$(PROFILE)
	@for f in $(CORE) $(NATIVE); do \
		echo "  CC32 $$f"; \
		$(CC_RISCV32) $(RV32FLAGS) \
			-include $(TRIM_OUT)/cmsis_config.h \
			-c $$f -o build/freestanding-$(PROFILE)/$$(basename $$f .c).o || exit 1; \
	done
	@echo "cmsis-rtos2: freestanding objects for profile $(PROFILE)"

clean:
	rm -rf build
