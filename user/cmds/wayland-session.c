#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void configure_environment(void)
{
    setenv("XDG_RUNTIME_DIR", "/tmp", 1);
    setenv("XKB_CONFIG_ROOT", "/bin/usr/share/X11/xkb", 1);
    setenv("XDG_CONFIG_DIRS", "/bin/etc/xdg", 1);
    setenv("LIBINPUT_QUIRKS_DIR", "/bin/share/libinput", 1);
    setenv("FONTCONFIG_FILE", "/bin/etc/fonts/fonts.conf", 1);
    setenv("WESTON_DATA_DIR", "/bin/share/weston", 1);
    setenv("XCURSOR_PATH", "/bin/share/icons", 1);
    setenv("XCURSOR_THEME", "Breeze", 1);
    setenv("WESTON_LIBINPUT_UDEV", "1", 1);
    setenv("WESTON_MODULE_MAP",
           "fbdev-backend.so=/bin/lib/libweston-9/fbdev-backend.so;"
           "kiosk-shell.so=/bin/lib/weston/kiosk-shell.so;"
           "desktop-shell.so=/bin/lib/weston/desktop-shell.so;"
           "weston-desktop-shell=/bin/libexec/weston-desktop-shell;"
           "weston-keyboard=/bin/libexec/weston-keyboard", 1);
    chmod("/tmp", 0700);
    mkdir("/tmp/fontconfig", 0700);
}

static void exec_weston(const char *shell)
{
    char shell_arg[64];
    snprintf(shell_arg, sizeof(shell_arg), "--shell=%s", shell);
    char *argv[] = {
        "weston", "--backend=fbdev-backend.so", "--seat=seat1", shell_arg,
        NULL,
    };
    execv("/bin/weston", argv);
    perror("wayland-session: weston");
    _exit(127);
}

static int wait_for_display(pid_t weston)
{
    struct stat st;
    for (int i = 0; i < 600000; i++) {
        if (stat("/tmp/wayland-0", &st) == 0) {
            for (int settle = 0; settle < 10; settle++)
                sched_yield();
            return 0;
        }
        if (kill(weston, 0) < 0 && errno == ESRCH)
            return -1;
        sched_yield();
    }
    return -1;
}

int main(void)
{
    configure_environment();
    pid_t weston = fork();
    if (weston < 0) {
        perror("wayland-session: fork weston");
        return 1;
    }
    if (weston == 0) {
        int log = open("/tmp/weston.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log >= 0) {
            dup2(log, STDOUT_FILENO);
            dup2(log, STDERR_FILENO);
            close(log);
        }
        exec_weston("desktop-shell.so");
    }
    if (wait_for_display(weston) < 0) {
        fprintf(stderr, "wayland-session: compositor did not become ready\n");
        kill(weston, SIGTERM);
        waitpid(weston, NULL, 0);
        return 1;
    }
    int status = 1;
    if (waitpid(weston, &status, 0) < 0)
        return 1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
