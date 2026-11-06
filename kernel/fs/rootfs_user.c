#include "fs/rootfs_user.h"

#ifdef CONFIG_RAMFS_USER

extern const unsigned char _binary_init_start[], _binary_init_end[];
extern const unsigned char _binary_mksh_start[], _binary_mksh_end[];
extern const unsigned char _binary_help_start[], _binary_help_end[];
extern const unsigned char _binary_ls_start[], _binary_ls_end[];
extern const unsigned char _binary_cat_start[], _binary_cat_end[];
extern const unsigned char _binary_ps_start[], _binary_ps_end[];
extern const unsigned char _binary_sleep_start[], _binary_sleep_end[];
extern const unsigned char _binary_timer_preempt_start[];
extern const unsigned char _binary_timer_preempt_end[];
extern const unsigned char _binary_timer_idle_start[];
extern const unsigned char _binary_timer_idle_end[];
#ifdef CONFIG_RAMFS_FPU_TESTS
extern const unsigned char _binary_fork_fpu_probe_start[], _binary_fork_fpu_probe_end[];
#endif
extern const unsigned char _binary_echo_start[], _binary_echo_end[];
extern const unsigned char _binary_uname_start[], _binary_uname_end[];
extern const unsigned char _binary_mkdir_start[], _binary_mkdir_end[];
extern const unsigned char _binary_rm_start[], _binary_rm_end[];
extern const unsigned char _binary_cp_start[], _binary_cp_end[];
extern const unsigned char _binary_sync_start[], _binary_sync_end[];
extern const unsigned char _binary_poweroff_start[], _binary_poweroff_end[];
extern const unsigned char _binary_reboot_start[], _binary_reboot_end[];

#ifdef CONFIG_STORAGE_READ_ONLY
extern const unsigned char _binary_storage_read_test_start[];
extern const unsigned char _binary_storage_read_test_end[];
#endif

#ifdef CONFIG_RAMFS_NATIVE_TESTS
extern const unsigned char _binary_native_futex_start[], _binary_native_futex_end[];
extern const unsigned char _binary_native_spawn_start[], _binary_native_spawn_end[];
extern const unsigned char _binary_native_input_start[], _binary_native_input_end[];
extern const unsigned char _binary_native_latency_start[], _binary_native_latency_end[];
extern const unsigned char _binary_cluster_threads_start[], _binary_cluster_threads_end[];
#endif

#ifdef CONFIG_RAMFS_RT_TESTS
extern const unsigned char _binary_sched_stress_start[], _binary_sched_stress_end[];
#endif

#ifdef CONFIG_RAMFS_ROUTER
extern const unsigned char _binary_wanctl_start[], _binary_wanctl_end[];
#endif

#define NET_PROGRAM(name) extern const unsigned char _binary_##name##_start[], _binary_##name##_end[]
#ifdef CONFIG_RAMFS_NET_TESTS
NET_PROGRAM(tcp_loopback_test);
NET_PROGRAM(udp_loopback_test);
NET_PROGRAM(ipv6_loopback_test);
NET_PROGRAM(net_lane_hostfwd_test);
NET_PROGRAM(udp_hostfwd_test);
NET_PROGRAM(bsd_scale_test);
NET_PROGRAM(bsd_tun_router_test);
#endif
#if defined(CONFIG_RAMFS_ROUTER) || defined(CONFIG_RAMFS_NET_TESTS)
NET_PROGRAM(pppoe);
NET_PROGRAM(pd_lan);
NET_PROGRAM(netctl);
#endif
#undef NET_PROGRAM

#ifdef CONFIG_RAMFS_MEDIA
extern const unsigned char _binary_videoplay_start[], _binary_videoplay_end[];
extern const unsigned char _binary_audioplay_start[], _binary_audioplay_end[];
#endif

static const unsigned char ramfs_shell_marker[] = "ram-only\n";

#define USER_ENTRY(path_, name_) \
    { path_, _binary_##name_##_start, \
      (size_t)(uintptr_t)_binary_##name_##_end, 0755 }

rootfs_overlay_entry_t g_rootfs_user_overlay[] = {
    { "/etc/a20-ramfs-shell", ramfs_shell_marker,
      sizeof(ramfs_shell_marker) - 1, 0644 },
    USER_ENTRY("/bin/init", init),
#ifdef CONFIG_RAMFS_ROUTER
    USER_ENTRY("/bin/wanctl", wanctl),
#endif
#ifdef CONFIG_RAMFS_NET_TESTS
    USER_ENTRY("/bin/tcp_loopback_test", tcp_loopback_test),
    USER_ENTRY("/bin/udp_loopback_test", udp_loopback_test),
    USER_ENTRY("/bin/ipv6_loopback_test", ipv6_loopback_test),
    USER_ENTRY("/bin/net_lane_hostfwd_test", net_lane_hostfwd_test),
    USER_ENTRY("/bin/udp_hostfwd_test", udp_hostfwd_test),
    USER_ENTRY("/bin/bsd_scale_test", bsd_scale_test),
    USER_ENTRY("/bin/bsd_tun_router_test", bsd_tun_router_test),
#endif
#if defined(CONFIG_RAMFS_ROUTER) || defined(CONFIG_RAMFS_NET_TESTS)
    USER_ENTRY("/bin/pppoe", pppoe),
    USER_ENTRY("/bin/pd-lan", pd_lan),
    USER_ENTRY("/bin/netctl", netctl),
#endif
#ifdef CONFIG_RAMFS_NATIVE_TESTS
    USER_ENTRY("/bin/native-futex", native_futex),
    USER_ENTRY("/bin/native-latency", native_latency),
    USER_ENTRY("/bin/cluster_threads", cluster_threads),
    USER_ENTRY("/bin/SPAWN.ELF", native_spawn),
    USER_ENTRY("/bin/INPUT.DAT", native_input),
#endif
#ifdef CONFIG_RAMFS_RT_TESTS
    USER_ENTRY("/bin/sched_stress", sched_stress),
#endif
    USER_ENTRY("/bin/mksh", mksh),
    USER_ENTRY("/bin/help", help),
    USER_ENTRY("/bin/ls", ls),
    USER_ENTRY("/bin/cat", cat),
    USER_ENTRY("/bin/ps", ps),
    USER_ENTRY("/bin/sleep", sleep),
    USER_ENTRY("/bin/timer_preempt", timer_preempt),
    USER_ENTRY("/bin/timer_idle", timer_idle),
#ifdef CONFIG_RAMFS_FPU_TESTS
    USER_ENTRY("/bin/fork_fpu_probe", fork_fpu_probe),
#endif
    USER_ENTRY("/bin/echo", echo),
    USER_ENTRY("/bin/uname", uname),
    USER_ENTRY("/bin/mkdir", mkdir),
    USER_ENTRY("/bin/rm", rm),
    USER_ENTRY("/bin/cp", cp),
    USER_ENTRY("/bin/sync", sync),
    USER_ENTRY("/bin/poweroff", poweroff),
    USER_ENTRY("/bin/reboot", reboot),
#ifdef CONFIG_RAMFS_MEDIA
    USER_ENTRY("/bin/videoplay", videoplay),
    USER_ENTRY("/bin/audioplay", audioplay),
#endif

#ifdef CONFIG_STORAGE_READ_ONLY
    USER_ENTRY("/bin/storage_read_test", storage_read_test),
#endif
};

const size_t g_rootfs_user_overlay_count =
    sizeof(g_rootfs_user_overlay) / sizeof(g_rootfs_user_overlay[0]);

void rootfs_user_overlay_init(void)
{
    /* Entry zero is the fixed-size marker; the remaining sizes hold end
     * addresses until the binary blobs have been assigned link addresses. */
    for (size_t i = 1; i < g_rootfs_user_overlay_count; i++) {
        rootfs_overlay_entry_t *entry = &g_rootfs_user_overlay[i];
        entry->size -= (size_t)(uintptr_t)entry->content;
    }
}

#else

rootfs_overlay_entry_t g_rootfs_user_overlay[] = {};
const size_t g_rootfs_user_overlay_count = 0;
void rootfs_user_overlay_init(void) {}

#endif
