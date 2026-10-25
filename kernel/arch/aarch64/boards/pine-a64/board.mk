ifneq ($(ARCH),aarch64)
$(error BOARD=pine-a64 requires ARCH=aarch64)
endif
ifneq ($(NR_CPUS),1)
$(error BOARD=pine-a64 currently supports NR_CPUS=1 only)
endif
ifeq ($(NOMMU),1)
$(error BOARD=pine-a64 currently requires MMU)
endif
ifeq ($(COOPERATIVE_BOOT),1)
$(error BOARD=pine-a64 requires architectural timer IRQs)
endif
