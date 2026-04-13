#include "../lib/libc.h"

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    printf("\033[1mA20OS Shell — Available Commands\033[0m\n\n");
    printf("\033[1mBuilt-in commands:\033[0m\n");
    printf("  cd [dir]             Change directory\n");
    printf("  exit [code]          Exit shell / power off\n");
    printf("  export VAR=val       Set environment variable\n");
    printf("  alias / unalias      Manage command aliases\n");
    printf("  history              Show command history\n");
    printf("\n\033[1mPrograms (in /bin):\033[0m\n");
    printf("  ls, cat, cp, rm, mkdir, touch, pwd\n");
    printf("  echo, env, clear, ps, aed\n");
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
