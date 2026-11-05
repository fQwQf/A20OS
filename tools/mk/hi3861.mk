# Hi3861 kernel A packaging and ROM serial programming.  The factory
# ROM/FlashBoot stays in place; the programming step only replaces kernel A.
HI3861_PORT ?= /dev/ttyUSB0
HI3861_BAUD ?= 115200
HI3861_LOADER ?= build/hi3861/loader.bin

.PHONY: hi3861-package hi3861-verify hi3861-prepare check-hi3861-image flash-hi3861
# Use through `tools/a20 package hi3861` (or pass the instance's ARCH/BOARD/
# PROFILE/NOMMU/ABI make variables explicitly); the Makefile selects the MCU
# source list and linker at parse time.
hi3861-package: kernel-only
	$(PYTHON) tools/boards/hi3861.py package --elf "$(KERNEL_ELF)"

hi3861-verify:
	$(PYTHON) tools/boards/hi3861.py verify

hi3861-prepare:
	$(PYTHON) tools/boards/hi3861_flash.py prepare --loader "$(HI3861_LOADER)"

check-hi3861-image: hi3861-package
	$(PYTHON) tools/boards/hi3861.py verify

flash-hi3861: hi3861-package
	$(PYTHON) tools/boards/hi3861_flash.py flash --port "$(HI3861_PORT)" \
	    --baud "$(HI3861_BAUD)" --loader "$(HI3861_LOADER)"
