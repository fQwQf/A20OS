# Explicit first-party network sources. External stacks have separate manifests.
A20_NET_SRC := \
    $(KERNEL_DIR)/net/core/stack.c \
    $(KERNEL_DIR)/net/backends/lwip/runtime.c \
    $(KERNEL_DIR)/net/socket/net_lock_probe.c \
    $(KERNEL_DIR)/net/socket/socket.c \
    $(KERNEL_DIR)/net/socket/socket_alg.c \
    $(KERNEL_DIR)/net/socket/socket_control.c \
    $(KERNEL_DIR)/net/socket/socket_file.c \
    $(KERNEL_DIR)/net/socket/socket_packet.c \
    $(KERNEL_DIR)/net/socket/socket_queue.c \
    $(KERNEL_DIR)/net/socket/socket_registry.c \
    $(KERNEL_DIR)/net/socket/socket_table.c \
    $(KERNEL_DIR)/net/socket/socket_unix.c \
    $(KERNEL_DIR)/net/control/net_config.c \
    $(KERNEL_DIR)/net/control/socket_netlink.c \
    $(KERNEL_DIR)/net/filter/netfilter.c \
    $(KERNEL_DIR)/net/filter/netfilter_nat.c \
    $(KERNEL_DIR)/net/filter/netfilter_rewrite.c \
    $(KERNEL_DIR)/net/crypto/md5.c \
    $(KERNEL_DIR)/net/backends/lwip/lwip_compat.c \
    $(KERNEL_DIR)/net/backends/lwip/lwip_concurrency.c \
    $(KERNEL_DIR)/net/backends/lwip/lwip_stack.c \
    $(KERNEL_DIR)/net/backends/lwip/net_lane.c \
    $(KERNEL_DIR)/net/backends/lwip/socket_inet.c
