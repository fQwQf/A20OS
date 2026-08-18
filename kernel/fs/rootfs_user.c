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
#ifdef CONFIG_STORAGE_READ_ONLY
extern const unsigned char _binary_storage_read_test_start[];
extern const unsigned char _binary_storage_read_test_end[];
#endif

static const unsigned char ramfs_shell_marker[] = "ram-only\n";

#define USER_ENTRY(path_, name_) \
    { path_, _binary_##name_##_start, \
      (size_t)(uintptr_t)_binary_##name_##_end, 0755 }

rootfs_overlay_entry_t g_rootfs_user_overlay[] = {
    { "/etc/a20-ramfs-shell", ramfs_shell_marker,
      sizeof(ramfs_shell_marker) - 1, 0644 },
    USER_ENTRY("/bin/init", init),
    USER_ENTRY("/bin/mksh", mksh),
    USER_ENTRY("/bin/help", help),
    USER_ENTRY("/bin/ls", ls),
    USER_ENTRY("/bin/cat", cat),
    USER_ENTRY("/bin/ps", ps),
    USER_ENTRY("/bin/sleep", sleep),
    USER_ENTRY("/bin/timer_preempt", timer_preempt),
    USER_ENTRY("/bin/timer_idle", timer_idle),
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
