/* mlibc-on-A20 spawn test child: prints argv/env info and exits with 42. */
#include <stdio.h>

int main(int argc, char **argv) {
	printf("mlibc child: alive argc=%d\n", argc);
	if (argc > 1)
		printf("mlibc child: argv1=%s\n", argv[1]);
	return 42;
}
