# The same physical base feeds assembler, C, ELF and EFI. Keep it outside the
# low firmware/ACPI area; the initial identity map covers only the first GiB.
X86_KERNEL_LOAD_ADDRESS ?= 0x02000000
# Quote a literal value as one shell argument before Python validates it.
x86_load_address_shellquote = '$(subst ','"'"',$(1))'
X86_KERNEL_LOAD_ADDRESS_CANONICAL := $(shell $(PYTHON) -c 'import re,sys; s=sys.argv[1]; n=int(s,0) if re.fullmatch(r"(?:0[xX][0-9a-fA-F]+|[0-9]+)",s) else 0; print(f"0x{n:08x}" if 0x200000 <= n <= 0x3e000000 and n % 0x200000 == 0 else "")' $(call x86_load_address_shellquote,$(X86_KERNEL_LOAD_ADDRESS)) 2>/dev/null)
ifeq ($(X86_KERNEL_LOAD_ADDRESS_CANONICAL),)
$(error X86_KERNEL_LOAD_ADDRESS must be a 2 MiB-aligned integer in [0x00200000,0x3e000000])
endif
ARCH_CONFIG_VARIANT := load$(X86_KERNEL_LOAD_ADDRESS_CANONICAL)
ARCH_CONFIG_CFLAGS := -DX86_KERNEL_LOAD_ADDRESS=$(X86_KERNEL_LOAD_ADDRESS_CANONICAL)
ARCH_CONFIG_LDFLAGS := -Wl,--defsym=PHYS_BASE=$(X86_KERNEL_LOAD_ADDRESS_CANONICAL)
