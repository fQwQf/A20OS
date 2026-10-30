# Explicit ownership manifests; never recursively collect backend sources.
A20_NET_COMMON_SRC := \
    $(KERNEL_DIR)/net/core/backend.c \
    $(KERNEL_DIR)/net/core/stack.c \
    $(KERNEL_DIR)/net/core/transport.c \
    $(KERNEL_DIR)/net/socket/net_lock_probe.c \
    $(KERNEL_DIR)/net/socket/address.c \
    $(KERNEL_DIR)/net/socket/socket.c \
    $(KERNEL_DIR)/net/socket/socket_alg.c \
    $(KERNEL_DIR)/net/socket/socket_control.c \
    $(KERNEL_DIR)/net/socket/socket_file.c \
    $(KERNEL_DIR)/net/socket/socket_packet.c \
    $(KERNEL_DIR)/net/socket/socket_queue.c \
    $(KERNEL_DIR)/net/socket/socket_registry.c \
    $(KERNEL_DIR)/net/socket/socket_table.c \
    $(KERNEL_DIR)/net/socket/socket_unix.c \
    $(KERNEL_DIR)/net/control/route.c \
    $(KERNEL_DIR)/net/control/net_config.c \
    $(KERNEL_DIR)/net/control/socket_netlink.c \
    $(KERNEL_DIR)/net/filter/netfilter.c \
    $(KERNEL_DIR)/net/filter/netfilter_nat.c \
    $(KERNEL_DIR)/net/filter/netfilter_rewrite.c \
    $(KERNEL_DIR)/net/crypto/md5.c

A20_NET_BACKEND_SRC :=
ifneq ($(filter lwip native,$(NET_STACK)),)
A20_NET_BACKEND_SRC := \
    $(KERNEL_DIR)/net/backends/lwip/input_guard.c \
    $(KERNEL_DIR)/net/backends/lwip/transport.c \
    $(KERNEL_DIR)/net/backends/lwip/runtime.c \
    $(KERNEL_DIR)/net/backends/lwip/socket_options.c \
    $(KERNEL_DIR)/net/backends/lwip/socket_state.c \
    $(KERNEL_DIR)/net/backends/lwip/interfaces.c \
    $(KERNEL_DIR)/net/backends/lwip/config.c \
    $(KERNEL_DIR)/net/backends/lwip/software_tx.c \
    $(KERNEL_DIR)/net/backends/lwip/lwip_compat.c \
    $(KERNEL_DIR)/net/backends/lwip/lwip_concurrency.c \
    $(KERNEL_DIR)/net/backends/lwip/lwip_stack.c \
    $(KERNEL_DIR)/net/backends/lwip/net_lane.c \
    $(KERNEL_DIR)/net/backends/lwip/socket_inet.c
endif
ifeq ($(NET_STACK),bsd)
A20_NET_BACKEND_SRC := \
    $(KERNEL_DIR)/net/backends/bsd/executor.c \
    $(KERNEL_DIR)/net/backends/bsd/runtime.c \
    $(KERNEL_DIR)/net/backends/bsd/options.c \
    $(KERNEL_DIR)/net/backends/bsd/transport.c
endif

A20_NET_SRC := $(A20_NET_COMMON_SRC) $(A20_NET_BACKEND_SRC)
