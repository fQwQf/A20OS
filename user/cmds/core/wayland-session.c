#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static void configure_environment(void)
{
    setenv("XDG_RUNTIME_DIR", "/tmp", 1);
    setenv("XKB_CONFIG_ROOT", "/bin/usr/share/X11/xkb", 1);
    setenv("XDG_CONFIG_DIRS", "/bin/etc/xdg", 1);
    setenv("XDG_CONFIG_HOME", "/bin/etc/xdg", 1);
    setenv("XDG_DATA_DIRS", "/bin/share", 1);
    setenv("LIBINPUT_QUIRKS_DIR", "/bin/share/libinput", 1);
    setenv("FONTCONFIG_FILE", "/bin/etc/fonts/fonts.conf", 1);
    setenv("WESTON_DATA_DIR", "/bin/share/weston", 1);
    setenv("XCURSOR_PATH", "/bin/share/icons", 1);
    setenv("XCURSOR_THEME", "Breeze", 1);
    unsetenv("WESTON_LIBINPUT_UDEV");
    setenv("WESTON_LIBINPUT_DEVICE", "/dev/event0", 1);
    setenv("WESTON_MODULE_MAP",
           "fbdev-backend.so=/bin/lib/libweston-9/fbdev-backend.so;"
           "kiosk-shell.so=/bin/lib/weston/kiosk-shell.so;"
           "desktop-shell.so=/bin/lib/weston/desktop-shell.so;"
           "weston-desktop-shell=/bin/libexec/weston-desktop-shell;"
           "weston-keyboard=/bin/libexec/weston-keyboard", 1);
    setenv("GDK_BACKEND", "wayland", 1);
    setenv("GDK_GL", "disable", 1);
    setenv("G_SLICE", "always-malloc", 1);
    setenv("G_DEBUG", "gc-friendly", 1);
    setenv("WAYLAND_DISPLAY", "wayland-0", 1);
    setenv("XDG_SESSION_TYPE", "wayland", 1);
    setenv("XDG_CURRENT_DESKTOP", "XFCE", 1);
    setenv("XFSM_VERBOSE", "1", 1);
    setenv("WLR_BACKENDS", "drm,libinput", 1);
    setenv("A20OS_NO_UDEV", "1", 1);
    setenv("A20OS_LIBINPUT_DEVICE", "/dev/event0", 1);
    setenv("WLR_RENDERER", "pixman", 1);
    setenv("WLR_DRM_NO_ATOMIC", "1", 1);
    setenv("WLR_NO_HARDWARE_CURSORS", "1", 1);
    setenv("WLR_DRM_DEVICES", "/dev/dri/card0", 1);
    setenv("GBM_BACKENDS_PATH", "/bin/lib/gbm", 1);
    setenv("SEATD_VTBOUND", "0", 1);
    setenv("A20OS_NO_SYSTEMD", "1", 1);
    setenv("A20OS_NO_HEADLESS", "1", 1);
    setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/tmp/dbus-session", 1);
}

static void settle_children(void)
{
    for (int i = 0; i < 400; i++)
        sched_yield();
}

static void settle_rounds(int rounds)
{
    for (int i = 0; i < rounds; i++)
        sched_yield();
}

static int wait_for_socket(const char *path)
{
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    for (int i = 0; i < 1000; i++) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd >= 0) {
            if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                close(fd);
                return 0;
            }
            close(fd);
        }
        settle_rounds(32);
    }
    return -1;
}

static void write_machine_id(void)
{
    int fd;
    mkdir("/etc", 0755);
    mkdir("/var", 0755);
    mkdir("/var/lib", 0755);
    mkdir("/var/lib/dbus", 0755);
    fd = open("/etc/machine-id", O_WRONLY | O_CREAT | O_TRUNC, 0444);
    if (fd >= 0) {
        static const char id[] = "a20os-xfce-machine-id-0000000000000001\n";
        write(fd, id, sizeof(id) - 1);
        close(fd);
    }
    fd = open("/var/lib/dbus/machine-id", O_WRONLY | O_CREAT | O_TRUNC, 0444);
    if (fd >= 0) {
        static const char id[] = "a20os-xfce-machine-id-0000000000000001\n";
        write(fd, id, sizeof(id) - 1);
        close(fd);
    }
}

static pid_t spawn_client(const char *path, const char *name)
{
    pid_t pid = fork();
    if (pid == 0) {
        fprintf(stderr, "[xfce] launching %s\n", name);
        execl(path, name, (char *)NULL);
        perror(path);
        _exit(127);
    }
    return pid;
}

static int run_clients(void)
{
    fprintf(stderr, "[xfce] waiting for Wayland listener\n");
    if (wait_for_socket("/tmp/wayland-0") < 0)
        fprintf(stderr, "[xfce] Wayland listener probe failed\n");
    else
        fprintf(stderr, "[xfce] Wayland listener ready\n");
    fprintf(stderr, "[xfce] launching clients after compositor handoff\n");
    pid_t xfconf = spawn_client("/bin/lib/xfce4/xfconf/xfconfd", "xfconfd");
    settle_rounds(256);
    pid_t panel = spawn_client("/bin/xfce4-panel", "xfce4-panel");
    pid_t desktop = spawn_client("/bin/xfdesktop", "xfdesktop");
    fprintf(stderr, "XFCE_DESKTOP_READY panel=%d desktop=%d\n", panel, desktop);
    int status = 1;
    if (panel > 0)
        waitpid(panel, &status, 0);
    if (xfconf > 0)
        kill(xfconf, SIGTERM);
    if (desktop > 0)
        kill(desktop, SIGTERM);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--clients") == 0)
        return run_clients();

    fprintf(stderr, "[xfce] wayland-session main\n");
    configure_environment();
    chmod("/tmp", 0700);
    mkdir("/tmp/fontconfig", 0700);
    write_machine_id();
    unlink("/tmp/dbus-session");
    pid_t bus = fork();
    if (bus < 0) {
        perror("wayland-session: fork dbus");
        return 1;
    }
    if (bus == 0) {
        execl("/bin/dbus-daemon", "/bin/dbus-daemon",
              "--config-file=/bin/etc/dbus-1/session.conf", "--nofork",
              (char *)NULL);
        perror("wayland-session: dbus-daemon");
        _exit(127);
    }
    settle_children();
    fprintf(stderr, "[xfce] session bus started\n");
    pid_t weston = fork();
    if (weston < 0) {
        perror("wayland-session: fork labwc");
        return 1;
    }
    if (weston == 0) {
        /* Start the small A20OS session supervisor only after labwc's output
         * and Wayland event loop are ready.  The full xfce4-session manager
         * requires distro failsafe-session data which is intentionally not
         * part of this image. */
        setenv("WAYLAND_DISPLAY", "wayland-0", 1);
        fprintf(stderr, "[xfce] exec labwc with XFCE startup\n");
        execl("/bin/labwc", "labwc", "--debug", "-S",
              "/bin/wayland-session --clients", (char *)NULL);
        perror("wayland-session: labwc");
        _exit(127);
    }
    fprintf(stderr, "AUTOSTART_DONE\n");
    int status = 1;
    if (waitpid(weston, &status, 0) < 0)
        return 1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
