#include <stdio.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    if (access("/etc/a20-ramfs-shell", F_OK) == 0) {
        printf("\033[1mA20OS RAM Shell - Available Commands\033[0m\n\n");
        printf("\033[1mBuilt-in commands:\033[0m\n");
        printf("  cd, echo, exit, export, history, pwd\n");
        printf("\n\033[1mPrograms in /bin:\033[0m\n");
        printf("  help, ls, cat, ps, sleep, timer_preempt, timer_idle, mksh\n");
        if (access("/bin/storage_read_test", X_OK) == 0)
            printf("  storage_read_test FILE [sample]\n");
        printf("\n\033[1mFilesystem:\033[0m\n");
        printf("  / is a writable RAM filesystem; reset restores it\n");
        return 0;
    }
    printf("\033[1mA20OS Shell — Available Commands\033[0m\n\n");
    printf("\033[1mBuilt-in commands:\033[0m\n");
    printf("  cd [dir]             Change directory\n");
    printf("  exit [code]          Exit shell / power off\n");
    printf("  export VAR=val       Set environment variable\n");
    printf("  alias / unalias      Manage command aliases\n");
    printf("  history              Show command history\n");
    printf("\n\033[1mPrograms (in /bin):\033[0m\n");
    printf("  ls, cat, cp, rm, mkdir, touch, pwd\n");
    printf("  echo, env, clear, ps, aed, audioplay\n");
    printf("  netstat, ping, udpmon, udpsend, wget\n");
    printf("  poweroff, reboot\n");
    printf("\n\033[1mFilesystems:\033[0m\n");
    printf("  /mnt/                FAT32\n");
    printf("  /mnt2/               ext4\n");
    printf("\n\033[1mShortcuts:\033[0m\n");
    printf("  Up/Down              Command history\n");
    printf("  Tab                  Auto-complete\n");
    printf("  Left/Right           Move cursor\n");
    printf("  Ctrl+C               Cancel line\n");
    printf("  Ctrl+D               Exit shell\n");
    printf("  Ctrl+L               Clear screen\n");
    printf("\n\033[1mSyntax:\033[0m\n");
    printf("  cmd1 | cmd2          Pipeline\n");
    printf("  cmd > file           Redirect output\n");
    printf("  cmd >> file          Append output\n");
    printf("  cmd < file           Redirect input\n");
    printf("  cmd1 && cmd2         And condition\n");
    printf("  cmd1 || cmd2         Or condition\n");
    printf("  $VAR, $?, $$         Variable expansion\n");
    return 0;
}
