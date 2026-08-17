/*
 * Typed control (handle_control GET_WINSIZE) replaces ioctl TIOCGWINSZ.
 * Verifies the versioned, capability-gated control op returns a sane
 * window size instead of the ioctl shim.
 * Prints MLIBC_WINSIZE: PASS on success.
 */
#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#include <errno.h>

int main(void) {
	struct winsize ws = {};
	int r = tcgetwinsize(STDOUT_FILENO, &ws);
	if (r < 0) {
		printf("MLIBC_WINSIZE: FAIL tcgetwinsize errno=%d\n", errno);
		return 1;
	}
	if (ws.ws_row < 1 || ws.ws_col < 1 || ws.ws_row > 200 || ws.ws_col > 400) {
		printf("MLIBC_WINSIZE: FAIL size row=%d col=%d\n", ws.ws_row, ws.ws_col);
		return 2;
	}
	printf("MLIBC_WINSIZE: PASS row=%d col=%d\n", ws.ws_row, ws.ws_col);
	return 0;
}
