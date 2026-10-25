ifneq ($(ARCH),aarch64)
$(error BOARD=quartz64-zero requires ARCH=aarch64)
endif
ifneq ($(NR_CPUS),1)
$(error BOARD=quartz64-zero currently supports NR_CPUS=1 only)
endif
ifeq ($(NOMMU),1)
$(error BOARD=quartz64-zero currently requires MMU)
endif
ifeq ($(COOPERATIVE_BOOT),1)
$(error BOARD=quartz64-zero requires architectural timer IRQs)
endif
