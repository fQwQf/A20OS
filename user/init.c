/*
 * PID 1 for the A20OS Linux-ABI root filesystem.
 *
 * Runs as the init process under the Linux ABI (the same musl/bionic-shaped
 * libc and syscall surface the rest of userland uses), so it deliberately
 * sticks to plain POSIX: no A20-native handle API, no EventQ, no privileged
 * kernel interface beyond reboot(2) and wait(2).  That keeps it usable against
 * a stock Alpine rootfs, which is the compatibility claim the whole ABI layer
 * exists to satisfy.
 *
 * Responsibilities, in order: mount the pseudo-filesystems the rootfs expects
 * (/proc, /sys, /dev), start the long-lived services, then either exec a shell
 * or sit in the wait/reap loop below.
 *
 * Signal handling: as PID 1 the kernel gives it no default dispositions for
 * unhandled signals, so the handler is the only thing standing between a
 * stray SIGTERM and an unreaped process tree.  It forwards to the recorded
 * child PIDs and sets got_signal, which the main loop treats as a shutdown
 * request.  kill() is async-signal-safe; the child PIDs are sig_atomic_t for
 * the same reason.
 */
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <unistd.h>

#include "runtime_setup.h"

static void xmkdir(const char *p) { mkdir(p, 0755); }

static void write_file(const char *path, const char *data, int mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd >= 0) { write(fd, data, strlen(data)); close(fd); }
}

static void append_if_dir(char *buf, size_t cap, const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        size_t len = strlen(buf);
        snprintf(buf + len, cap - len, "%s%s", len ? ":" : "", path);
    }
}

static pid_t shell_pid;
static pid_t telnet_pid;
static pid_t vmd_pid;
static pid_t wan_pid;
static volatile sig_atomic_t got_signal;
static volatile sig_atomic_t shutdown_command = RB_POWER_OFF;

static void sig_handler(int sig)
{
    shutdown_command = sig == SIGTERM ? RB_AUTOBOOT : RB_POWER_OFF;
    got_signal = 1;
    if (shell_pid > 0)
        kill(shell_pid, SIGTERM);
    if (telnet_pid > 0)
        kill(telnet_pid, SIGTERM);
    if (vmd_pid > 0)
        kill(vmd_pid, SIGTERM);
    if (wan_pid > 0)
        kill(wan_pid, SIGTERM);
}

static int vm_manager_enabled(void)
{
    char command[2048];
    int fd = open("/proc/cmdline", O_RDONLY);
    if (fd < 0) return 0;
    ssize_t size = read(fd, command, sizeof(command) - 1);
    close(fd);
    if (size <= 0) return 0;
    command[size] = 0;
    char *save = NULL;
    for (char *word = strtok_r(command, " \n\t", &save); word;
         word = strtok_r(NULL, " \n\t", &save))
        if (!strcmp(word, "a20.vmd=1")) return 1;
    return 0;
}

static void start_wan(char **envp)
{
    const char key[] = "a20.wan_config=";
    char command[2048];
    int fd = open("/proc/cmdline", O_RDONLY);
    if (fd < 0)
        return;
    ssize_t size = read(fd, command, sizeof(command) - 1);
    close(fd);
    if (size <= 0)
        return;
    command[size] = 0;
    char *save = NULL;
    for (char *word = strtok_r(command, " \n\t", &save); word;
         word = strtok_r(NULL, " \n\t", &save)) {
        if (strncmp(word, key, sizeof(key) - 1) != 0)
            continue;
        char *path = word + sizeof(key) - 1;
        if (*path != '/') {
            fprintf(stderr, "[init] a20.wan_config requires an absolute path\n");
            return;
        }
        wan_pid = fork();
        if (wan_pid == 0) {
            char *argv[] = {"/bin/wanctl", path, NULL};
            execve(argv[0], argv, envp);
            perror("execve wanctl");
            _exit(127);
        }
        if (wan_pid < 0) {
            perror("fork wanctl");
            wan_pid = 0;
        }
        return;
    }
}

static void setup_console(void)
{
    int fd = open("/dev/console", O_RDWR | O_NOCTTY);
    if (fd < 0)
        return;
    dup2(fd, STDIN_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > 2)
        close(fd);
}

static void setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);

    sa.sa_handler = SIG_IGN;
    sigaction(SIGHUP, &sa, NULL);
}

/* The platform init keeps only A20-specific service and root handoff policy.
 * Generic commands and the shell are the upstream BusyBox implementation. */
static void setup_upstream_tools(void)
{
    const char *binary = access("/bin/busybox", X_OK) == 0 ? "/bin/busybox" : "/a20/busybox";
    if (binary[1] == 'a') {
        xmkdir("/bin");
        symlink("/a20/sh", "/bin/sh");
        symlink("/a20/busybox", "/bin/busybox");
    }
    printf("[init] BusyBox user tools ready\n");
    /* FAT32 images already carry small applet scripts. Only the writable
     * RAMFS needs BusyBox to install its standard command links at boot. */
    if (access("/etc/a20-ramfs-shell", F_OK) != 0)
        return;
    const char *dirs[] = {"/bin", "/sbin", "/usr", "/usr/bin", "/usr/sbin", NULL};
    for (const char **dir = dirs; *dir; dir++)
        xmkdir(*dir);
    pid_t pid = fork();
    if (pid == 0) {
        char *args[] = {"busybox", "--install", "-s", NULL};
        execv(binary, args);
        _exit(127);
    }
    if (pid > 0)
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
    symlink("/bin/busybox", "/bin/poweroff");
    symlink("/bin/busybox", "/bin/reboot");
}

static void reap_children(void)
{
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

static void stop_shell_child(pid_t *child)
{
    if (*child <= 0) return;
    kill(*child, SIGTERM);
    for (int i = 0; i < 10; i++) {
        pid_t reaped = waitpid(*child, NULL, WNOHANG);
        if (reaped == *child || (reaped < 0 && errno == ECHILD)) {
            *child = 0;
            return;
        }
        usleep(10000);
    }
    /* An interactive shell can ignore SIGTERM. Its script descriptor must
     * close before filesystem shutdown can quiesce a FAT root. */
    kill(*child, SIGKILL);
    while (waitpid(*child, NULL, 0) < 0 && errno == EINTR) {}
    *child = 0;
}

/*
 * Distro rootfs boot mode.  When a compatible distro image (Alpine + the
 * stage-2 init overlay) is mounted at /extra, A20OS stops acting as a
 * self-contained userspace and becomes just the kernel underneath that
 * distro: we chroot(2) into /extra and exec /sbin/init, which brings up the
 * login/device services (dbus / elogind / seatd / udevd) and the XFCE
 * Wayland session.  The kernel only provides the Linux ABI + /dev + /proc +
 * /sys; the distro owns the userspace.
 */
static void do_shutdown(void);

static int copy_embedded_tool(const char *name, mode_t mode)
{
    char source[128], destination[128];
    snprintf(source, sizeof(source), "/bin/%s", name);
    snprintf(destination, sizeof(destination), "/extra/a20/%s", name);
    int input = open(source, O_RDONLY | O_CLOEXEC);
    if (input < 0)
        return -1;
    int output = open(destination, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (output < 0) {
        close(input);
        return -1;
    }
    char buffer[16384];
    int result = 0;
    for (;;) {
        ssize_t count = read(input, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            result = count < 0 ? -1 : 0;
            break;
        }
        ssize_t offset = 0;
        while (offset < count) {
            ssize_t written = write(output, buffer + offset, count - offset);
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0) {
                result = -1;
                break;
            }
            offset += written;
        }
        if (result)
            break;
    }
    int saved = errno;
    if (close(output) < 0 && !result) {
        saved = errno;
        result = -1;
    }
    close(input);
    errno = saved;
    return result;
}

static int detect_distro_rootfs(void)
{
    struct stat st;
    if (access("/extra/etc/a20-distro", F_OK) != 0)
        return 0;
    if (stat("/extra/sbin/init", &st) != 0 || !S_ISREG(st.st_mode))
        return 0;
    return 1;
}

static void enter_distro_rootfs(void)
{
    printf("[init] distro mode: entering rootfs at /extra\n");
    int have_wifi = access("/bin/rtw89d", X_OK) == 0;
    int have_desktop_tools = access("/bin/session-log.py", F_OK) == 0;
    int managed_init = access("/extra/etc/a20-openrc", F_OK) == 0;
    if (have_wifi || have_desktop_tools) {
        xmkdir("/extra/a20");
        /* Stage opt-in tools in RAM, independently of the radio transport. */
        if (mount("tmpfs", "/extra/a20", "tmpfs", 0, NULL) < 0) {
            perror("prepare embedded tools mount");
            have_wifi = have_desktop_tools = 0;
        }
    }
    if (have_wifi) {
        if (copy_embedded_tool("rtw89d", 0755) < 0 ||
            copy_embedded_tool("wifi", 0755) < 0 ||
            copy_embedded_tool("wifi-dhcp", 0755) < 0 ||
            copy_embedded_tool("rtw89-notices", 0444) < 0 ||
            copy_embedded_tool("wifi-gui", 0755) < 0 ||
            copy_embedded_tool("wifi_backend.py", 0644) < 0 ||
            copy_embedded_tool("wifi_portal.py", 0644) < 0 ||
            copy_embedded_tool("a20-wifi.desktop", 0755) < 0) {
            perror("prepare embedded Wi-Fi tools");
            have_wifi = 0;
        } else {
            (void)symlink("/a20/wifi", "/extra/usr/local/bin/wifi");
            (void)symlink("/a20/wifi-gui", "/extra/usr/local/bin/wifi-gui");
            (void)symlink("/a20/a20-wifi.desktop", "/extra/usr/share/applications/a20-wifi.desktop");
            xmkdir("/extra/root/Desktop");
            (void)symlink("/a20/a20-wifi.desktop", "/extra/root/Desktop/a20-wifi.desktop");
        }
    }
    if (have_desktop_tools) {
        if (copy_embedded_tool("input-probe.py", 0644) < 0 ||
            copy_embedded_tool("session-log.py", 0644) < 0 ||
            copy_embedded_tool("session-power.py", 0755) < 0 ||
            copy_embedded_tool("90-a20-ibus.gschema.override", 0644) < 0 ||
            copy_embedded_tool("90-libinput-fuzz-override.rules", 0644) < 0) {
            perror("prepare embedded desktop tools");
            have_desktop_tools = 0;
        } else {
            xmkdir("/extra/etc/udev");
            xmkdir("/extra/etc/udev/rules.d");
            (void)symlink("/a20/90-libinput-fuzz-override.rules",
                          "/extra/etc/udev/rules.d/90-libinput-fuzz-override.rules");
        }
    }
    if (chdir("/extra") != 0 || chroot("/extra") != 0) {
        perror("chroot distro rootfs");
        do_shutdown();
    }
    if (have_wifi && !managed_init) {
        xmkdir("/var/log");
        xmkdir("/var/log/a20");
        pid_t pid = fork();
        if (pid == 0) {
            setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);
            (void)rename("/var/log/a20/wifi.log", "/var/log/a20/wifi-prev.log");
            int log = open("/var/log/a20/wifi.log", O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (log >= 0) {
                dup2(log, STDOUT_FILENO);
                dup2(log, STDERR_FILENO);
                if (log > STDERR_FILENO)
                    close(log);
            }
            execl("/a20/rtw89d", "rtw89d", (char *)NULL);
            perror("start Wi-Fi service");
            _exit(127);
        }
        if (pid < 0)
            perror("fork Wi-Fi service");
    }
    if (have_desktop_tools && !managed_init) {
        xmkdir("/var/log");
        xmkdir("/var/log/a20");
        pid_t probe_pid = fork();
        if (probe_pid == 0) {
            int log = open("/var/log/a20/touchpad.log", O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (log >= 0) {
                dup2(log, STDOUT_FILENO);
                dup2(log, STDERR_FILENO);
                if (log > STDERR_FILENO)
                    close(log);
            }
            execl("/usr/bin/python3", "python3", "/a20/input-probe.py", (char *)NULL);
            _exit(127);
        }
        if (probe_pid < 0)
            perror("fork touchpad diagnostic");
    }
    /* The distro owns PID1. Its OpenRC instance also owns Wi-Fi and session
     * services; the ramfs process only stages platform tools and execs it. */
    char *distro_init = "/sbin/init";
    char *distro_argv[] = { distro_init, NULL };
    char *distro_envp[] = {
        "PATH=/usr/sbin:/usr/bin:/sbin:/bin",
        "HOME=/root", "SHELL=/bin/bash", "TERM=linux",
        "XDG_RUNTIME_DIR=/run/user/0", NULL
    };
    execve(distro_init, distro_argv, distro_envp);
    perror("execve /sbin/init");
    do_shutdown();
}

static void do_shutdown(void)
{
    sync();
    puts(shutdown_command == RB_AUTOBOOT ? "Rebooting system." :
         "System is going down for power-off NOW.");
    printf("[init] shutting down\n");
    reboot(shutdown_command);
    for (;;) __asm__ volatile("");
}

int main(void)
{
    setsid();
    setup_console();
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    setup_signals();
    setup_upstream_tools();

    if (detect_distro_rootfs())
        enter_distro_rootfs();

    ioctl(STDIN_FILENO, TIOCSCTTY, 0);

#if defined(__powerpc64__)
    char *argv[] = {"sh", NULL};
    char *envp[] = {"PATH=/bin", "HOME=/", "SHELL=/bin/sh",
                    "TERM=vt100", NULL};
    execve("/bin/sh", argv, envp);
    perror("execve sh");
    do_shutdown();
#else
    int external_root =
        (access("/a20/etc/external-root", F_OK) == 0 ||
         access("/bin/etc/external-root", F_OK) == 0);
    int ramfs_shell = (access("/etc/a20-ramfs-shell", F_OK) == 0);

    /*
     * External-root builds retain the bootstrap ramfs as / and mount the
     * attached root disk at /mnt.  The bootstrap init remains available
     * when recovery or mounting fails.  Keep the original setup for
     * interactive images.
     */
    if (!external_root && !ramfs_shell)
        setup_runtime_links();

    xmkdir("/tmp");
    xmkdir("/tmp/sysinfo");
    write_file("/tmp/sysinfo/model",
               ramfs_shell ? "A20OS RAM-only shell\n" :
                             "A20OS Virtual Machine\n", 0644);

    char path_val[512];
    if (external_root)
        strcpy(path_val, "/a20");
    else
        strcpy(path_val, "/bin");
    /* /extra is the extra image: programs at its root, /extra/bin wrappers,
     * package prefixes under /extra (see tools/mk/targets-extra.mk). */
    static const char *path_dirs[] = {
        "/sbin", "/usr/bin", "/usr/sbin", "/extra", "/extra/bin",
        NULL,
    };
    for (const char **p = path_dirs; *p; p++)
        append_if_dir(path_val, sizeof(path_val), *p);

    char ld_val[256] = "";
    /* /extra/glibc/lib is staged by the extra image for its glibc-linked
     * packages (rustc/cargo/lamina); the rest are standard locations. */
    static const char *ld_dirs[] = {
        "/extra/glibc/lib", "/extra/lib",
        "/glibc/lib", "/usr/lib", "/lib", "/bin/lib", NULL,
    };
    for (const char **p = ld_dirs; *p; p++)
        append_if_dir(ld_val, sizeof(ld_val), *p);
    if (!ld_val[0]) strcpy(ld_val, "/lib:/bin/lib");

    const char *shell_path = external_root ? "/a20/sh" : "/bin/sh";
    char path_env[576], ld_env[320], shell_env[64];
    snprintf(path_env, sizeof(path_env), "PATH=%s", path_val);
    snprintf(ld_env, sizeof(ld_env), "LD_LIBRARY_PATH=%s", ld_val);
    snprintf(shell_env, sizeof(shell_env), "SHELL=%s", shell_path);

    char *legacy_envp[] = {
        path_env, ld_env, "HOME=/", shell_env, "TERM=vt100", "PS1=# ",
        "XDG_RUNTIME_DIR=/tmp", "WAYLAND_DISPLAY=wayland-0",
        "XDG_CONFIG_DIRS=/bin/etc/xdg",
        "XDG_CONFIG_HOME=/bin/etc/xdg", NULL
    };
    char *minimal_envp[] = {
        path_env, "HOME=/", shell_env, "TERM=vt100", "PS1=# ", NULL
    };
    char **envp = external_root ? minimal_envp : legacy_envp;

    if (!external_root)
        start_wan(envp);

    if (!external_root && !ramfs_shell && vm_manager_enabled()) {
        vmd_pid = fork();
        if (vmd_pid == 0) {
            char *vmd_argv[] = { "a20-vmd", NULL };
            execve("/bin/a20-vmd", vmd_argv, envp);
            perror("execve a20-vmd");
            _exit(127);
        }
        if (vmd_pid < 0) { perror("fork a20-vmd"); vmd_pid = 0; }
    }

    if (!external_root && !ramfs_shell) {
        telnet_pid = fork();
        if (telnet_pid == 0) {
            char *telnet_argv[] = { "telnetd", "2323", NULL };
            execve("/bin/telnetd", telnet_argv, envp);
            _exit(127);
        }
        if (telnet_pid < 0) {
            perror("fork telnetd");
            telnet_pid = 0;
        }
    }

    char *argv[] = {"sh", NULL};

    printf("[init] forking...\n");
    shell_pid = fork();
    if (shell_pid == 0) {
        printf("[init] fork=0, calling execve sh\n");
        execve(shell_path, argv, envp);
        perror("execve sh");
        _exit(127);
    }
    if (shell_pid < 0) {
        printf("[init] fork failed with %d, errno %d\n", shell_pid, errno);
        if (errno == EINVAL) {
            /* NOMMU kernels do not implement fork().  Replace this
             * process with the shell so the system still reaches an
             * interactive prompt. */
            printf("[init] errno=EINVAL, calling execve sh\n");
            execve(shell_path, argv, envp);
            perror("execve sh");
        } else {
            perror("fork");
        }
        do_shutdown();
    }

    for (;;) {
        int status;
        pid_t w = waitpid(-1, &status, 0);
        if (w < 0) {
            if (got_signal)
                break;
            continue;
        }
        if (w == shell_pid) {
            printf("[init] shell exited: status=%d\n", status);
            shell_pid = 0;
            break;
        }
        if (w == wan_pid) {
            wan_pid = 0;
            printf("[init] wanctl exited\n");
        }
        if (w == vmd_pid) {
            vmd_pid = 0;
            printf("[init] a20-vmd exited; guest processes receive parent-death SIGKILL\n");
        }
        if (w == telnet_pid) {
            telnet_pid = 0;
            printf("[init] telnetd exited\n");
        }
    }

    stop_shell_child(&shell_pid);
    stop_shell_child(&telnet_pid);
    if (vmd_pid > 0) {
        kill(vmd_pid, SIGTERM);
        while (waitpid(vmd_pid, NULL, 0) < 0 && errno == EINTR) ;
        vmd_pid = 0;
    }
    if (wan_pid > 0) {
        kill(wan_pid, SIGTERM);
        while (waitpid(wan_pid, NULL, 0) < 0 && errno == EINTR) ;
        wan_pid = 0;
    }
    reap_children();
    do_shutdown();
#endif
}
