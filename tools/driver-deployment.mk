# Driver deployment policy.
#
# `generic` keeps optional hardware outside the kernel image and activates it
# from a DriverStore. `embedded` links its complete driver set into the image.
# The profile is intentionally not named after a board: STM32 is merely the
# current embedded default.

DEFAULT_DRIVER_DEPLOYMENT := generic
# MCU targets and platforms without a drvmod cross-toolchain (no .a20drv
# packages can be built for them) statically link the complete driver set.
ifeq ($(filter $(ARCH),armv7m ppc64le),$(ARCH))
DEFAULT_DRIVER_DEPLOYMENT := embedded
endif

DRIVER_DEPLOYMENT ?= $(DEFAULT_DRIVER_DEPLOYMENT)
ifneq ($(filter $(DRIVER_DEPLOYMENT),generic embedded),$(DRIVER_DEPLOYMENT))
$(error Unsupported DRIVER_DEPLOYMENT='$(DRIVER_DEPLOYMENT)'; supported: generic, embedded)
endif

ifeq ($(DRIVER_DEPLOYMENT),generic)
DRIVER_DEPLOYMENT_CPPFLAGS := -DCONFIG_DRIVER_DEPLOYMENT_GENERIC
else
DRIVER_DEPLOYMENT_CPPFLAGS := -DCONFIG_DRIVER_DEPLOYMENT_EMBEDDED
endif
