# A RAM-only package, independent of the generic Linux booti bundle.
.PHONY: ls2k0300-bundle
ls2k0300-bundle: kernel-only
	@test "$(ARCH):$(BOARD)" = "loongarch64:ls2k0300" || { echo 'use ARCH=loongarch64 BOARD=ls2k0300' >&2; exit 2; }
	python3 tools/mk_ls2k0300_bundle.py $(KERNEL_ELF) $(KERNEL_BIN) build/ls2k0300/handoff
