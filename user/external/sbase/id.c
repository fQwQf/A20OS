/* See LICENSE file for copyright and license details. */
#include <stdio.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>

#include "util.h"

static void
usage(void)
{
	eprintf("usage: %s [-Ggnru] [user]\n", argv0);
}

static int show_group_names = 0;
static int show_id = 0;
static int show_real = 0;
static int show_name = 0;

int
main(int argc, char *argv[])
{
	uid_t uid;
	gid_t gid;
	struct passwd *pw;
	struct group *gr;

	argv0 = *argv, argv0 ? (argc--, argv++) : (void *)0;

	while (argc > 0 && **argv == '-') {
		const char *p = *argv + 1;
		argc--, argv++;
		for (; *p; p++) {
			switch (*p) {
			case 'G': show_group_names = 1; break;
			case 'g': show_id = 1; break;
			case 'n': show_name = 1; break;
			case 'r': show_real = 1; break;
			case 'u': break;
			default: usage();
			}
		}
	}

	if (argc > 1)
		usage();

	uid = show_real ? getuid() : geteuid();
	gid = show_real ? getgid() : getegid();

	if (show_id) {
		if (show_name) {
			pw = getpwuid(gid);
			if (pw)
				puts(pw->pw_name);
			else
				printf("%d\n", gid);
		} else {
			printf("%d\n", gid);
		}
	} else if (show_group_names) {
		if (show_name) {
			gr = getgrgid(gid);
			if (gr)
				puts(gr->gr_name);
			else
				printf("%d\n", gid);
		} else {
			printf("%d\n", gid);
		}
	} else {
		pw = getpwuid(uid);
		gr = getgrgid(gid);
		printf("uid=%d(%s) gid=%d(%s)\n",
		       uid, pw ? pw->pw_name : "unknown",
		       gid, gr ? gr->gr_name : "unknown");
	}

	return fshut(stdout, "<stdout>");
}
