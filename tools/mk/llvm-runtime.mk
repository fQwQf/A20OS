# Included after the normal default goal has been selected.
$(LLVM_USER_BUILTINS): $(A20_TOOLCHAIN_ROOT)/tools/toolchain/build-builtins.py $(A20_TOOLCHAIN_ROOT)/tools/toolchain/include/assert.h
	$(PYTHON) $(A20_TOOLCHAIN_ROOT)/tools/toolchain/build-builtins.py --arch $(ARCH) --kind user --output $@
$(LLVM_KERNEL_BUILTINS): $(A20_TOOLCHAIN_ROOT)/tools/toolchain/build-builtins.py $(A20_TOOLCHAIN_ROOT)/tools/toolchain/include/assert.h
	$(PYTHON) $(A20_TOOLCHAIN_ROOT)/tools/toolchain/build-builtins.py --arch $(ARCH) --kind $(LLVM_KERNEL_RT_KIND) --output $@
