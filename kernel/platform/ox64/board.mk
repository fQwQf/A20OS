# NOMMU still runs the RV64 D0 in S-mode through OpenSBI.
ifeq ($(NOMMU),1)
LDFLAGS_NOMMU := -Wl,--defsym=VIRT_BASE=0x50200000
endif
