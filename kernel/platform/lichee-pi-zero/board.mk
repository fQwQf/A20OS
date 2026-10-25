ifneq ($(ARCH),arm32)
$(error BOARD=lichee-pi-zero requires ARCH=arm32)
endif
ifneq ($(NR_CPUS),1)
$(error V3s is single-core; BOARD=lichee-pi-zero requires NR_CPUS=1)
endif
ifeq ($(NOMMU),1)
$(error BOARD=lichee-pi-zero requires the ARMv7-A MMU)
endif
