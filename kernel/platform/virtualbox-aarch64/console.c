#ifdef CONFIG_AARCH64

#include "core/defs.h"
#include "core/stdio.h"
#include "core/string.h"
#include "drivers/char/uart.h"
#include "fs/vfs.h"
#include "mm/frame.h"

#define VBOX_CONSOLE_LINE_MAX 80

static void vbox_console_prompt(void)
{
    uart_puts("a20os> ");
}

static const char *vbox_console_arg(char *line, const char *command)
{
    char *arg = line + strlen(command);
    while (*arg == ' ')
        arg++;
    return *arg ? arg : "/";
}

static void vbox_console_ls(const char *path)
{
    unsigned char buffer[512];
    int fd = vfs_open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        printf("ls: cannot open %s: %d\n", path, fd);
        return;
    }

    int n;
    while ((n = vfs_getdents64(fd, buffer, sizeof(buffer))) > 0) {
        size_t offset = 0;
        while (offset < (size_t)n) {
            vfs_dirent64_t *entry = (vfs_dirent64_t *)(buffer + offset);
            if (entry->d_reclen == 0 || offset + entry->d_reclen > (size_t)n)
                break;
            printf("%s%s  ", entry->d_name,
                   entry->d_type == 4 ? "/" : "");
            offset += entry->d_reclen;
        }
    }
    if (n < 0)
        printf("\nls: read error: %d\n", n);
    else
        uart_puts("\n");
    vfs_close(fd);
}

static void vbox_console_cat(const char *path)
{
    char buffer[128];
    int fd = vfs_open(path, O_RDONLY, 0);
    if (fd < 0) {
        printf("cat: cannot open %s: %d\n", path, fd);
        return;
    }
    int n;
    while ((n = vfs_read(fd, buffer, sizeof(buffer))) > 0) {
        for (int i = 0; i < n; i++)
            uart_putc(buffer[i]);
    }
    if (n < 0)
        printf("cat: read error: %d\n", n);
    vfs_close(fd);
}

static void vbox_console_execute(char *line)
{
    if (line[0] == '\0')
        return;

    if (strcmp(line, "help") == 0) {
        uart_puts("commands: help status mem ls [path] cat <path> clear reboot poweroff\n");
    } else if (strcmp(line, "status") == 0) {
        uart_puts("A20OS VirtualBox ARM64 serial bring-up\n");
        uart_puts("timer: software fallback; disk/network/gui: unavailable\n");
    } else if (strcmp(line, "mem") == 0) {
        printf("memory: %lu/%lu frames free (%lu MiB)\n",
               (unsigned long)pfa.free_frames,
               (unsigned long)pfa.total_frames,
               (unsigned long)(pfa.free_frames * PAGE_SIZE / 1024 / 1024));
    } else if (strncmp(line, "ls", 2) == 0 && (line[2] == '\0' || line[2] == ' ')) {
        vbox_console_ls(vbox_console_arg(line, "ls"));
    } else if (strncmp(line, "cat ", 4) == 0) {
        vbox_console_cat(vbox_console_arg(line, "cat"));
    } else if (strcmp(line, "clear") == 0) {
        uart_puts("\033[2J\033[H");
    } else if (strcmp(line, "reboot") == 0) {
        uart_puts("rebooting\n");
        firmware_reboot();
    } else if (strcmp(line, "poweroff") == 0) {
        uart_puts("powering off\n");
        firmware_shutdown();
    } else {
        printf("unknown command: %s\n", line);
    }
}

void vbox_serial_console(void)
{
    char line[VBOX_CONSOLE_LINE_MAX];
    size_t length = 0;
    int previous_was_cr = 0;

    uart_puts("\nVirtualBox serial console ready. Type 'help'.\n");
    vbox_console_prompt();

    for (;;) {
        int c = firmware_console_getchar();
        if (c < 0) {
            arch_local_irq_enable();
            arch_cpu_relax();
            continue;
        }

        if (c == '\n' && previous_was_cr) {
            previous_was_cr = 0;
            continue;
        }
        previous_was_cr = c == '\r';

        if (c == '\r' || c == '\n') {
            uart_puts("\n");
            line[length] = '\0';
            vbox_console_execute(line);
            length = 0;
            vbox_console_prompt();
            continue;
        }

        if (c == '\b' || c == 0x7f) {
            if (length) {
                length--;
                uart_puts("\b \b");
            }
            continue;
        }

        if (c >= 0x20 && c < 0x7f && length + 1 < sizeof(line)) {
            line[length++] = (char)c;
            uart_putc((char)c);
        }
    }
}

#endif /* CONFIG_AARCH64 */
