/* A pointer-signature hint is not an access bit, so a caller that passes one
 * has to get back the mapping it asked for rather than an EINVAL it can do
 * nothing with.
 *
 * The two entry points differ, and the difference is the point.  mmap never
 * validated prot against a known mask -- it reads only bits 1/2/4 -- so a hint
 * passed there was already ignored rather than refused.  mprotect did validate,
 * and a bare `prot & ~(R|W|X)` test turned a hint into an EINVAL whose only
 * remedy the caller does not have, since the hint arrives in the same argument
 * as the access rights.  So mprotect is where the behaviour is asserted, and
 * mmap is checked only to confirm the hint does not disturb it. */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>

#define PAGES 2
#define HINT  0x10   /* PROT_BTI: restated rather than taken from the shared
                     * header, so the probe still means something if that
                     * header ever stops carrying it. */
#define HINT2 0x20   /* PROT_MTE */

static int failures;

static void check_success(const char *what, int rc) {
	/* No column padding: a run of spaces inside a line is easy to mistype
	 * into an expected marker and hard to read when it is wrong. */
	printf("PROT_HINT: mprotect[%s] = %s, want %s\n", what,
	       rc == 0 ? "ok" : "MISMATCH",
	       "success");
	if (rc != 0)
		failures++;
}

static void check_einval(const char *what, int rc, int error) {
	int ok = rc == -1 && error == EINVAL;
	printf("PROT_HINT: mprotect[%s] = %s, want EINVAL (errno=%d)\n",
	       what, ok ? "ok" : "MISMATCH", error);
	if (!ok)
		failures++;
}

/* mmap: the hint must change nothing.  Compared rather than asserted, because
 * whether the access rights underneath are allowed at all is a W^X policy
 * decision that this probe has no business making. */
static void mmap_hint_unchanged(const char *what, int base, int hint) {
	int plain, hinted;

	void *a = mmap(NULL, PAGES * 4096, base, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	plain = (a != MAP_FAILED);
	if (plain)
		munmap(a, PAGES * 4096);

	void *b = mmap(NULL, PAGES * 4096, base | hint,
	               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	hinted = (b != MAP_FAILED);
	if (hinted)
		munmap(b, PAGES * 4096);

	printf("PROT_HINT: mmap[%s]: plain=%s, hinted=%s, %s\n", what,
	       plain ? "ok" : "refused", hinted ? "ok" : "refused",
	       plain == hinted ? "unchanged" : "CHANGED");
	if (plain != hinted)
		failures++;
}

/* mprotect: the strict path.  Each case runs on a fresh mapping so that one
 * refusal cannot leave the next case without something to change. */
static void mprotect_case(const char *what, int prot, int want) {
	void *p = mmap(NULL, PAGES * 4096, PROT_READ | PROT_WRITE,
	               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		printf("PROT_HINT: mprotect[%s]: SETUP FAILED\n", what);
		failures++;
		return;
	}
	errno = 0;
	int r = mprotect(p, PAGES * 4096, prot);
	int saved_errno = errno;
	if (want)
		check_success(what, r);
	else
		check_einval(what, r, saved_errno);

	/* Successful changes and rejected changes must both leave a writable
	 * mapping: the latter must preserve the old RW permissions. */
	volatile char *q = p;
	*q = 'x';
	if (*q != 'x') {
		printf("PROT_HINT: mprotect[%s]: mapping is not writable\n", what);
		failures++;
	}
	munmap(p, PAGES * 4096);
}

int main(void) {
	int rw = PROT_READ | PROT_WRITE;

	printf("PROT_HINT: begin\n");

	mmap_hint_unchanged("RW", rw, HINT);
	mmap_hint_unchanged("RWX", rw | PROT_EXEC, HINT);
	mmap_hint_unchanged("RW", rw, HINT2);

	mprotect_case("RW (no hint)", rw, 1);
	mprotect_case("RW | PROT_BTI", rw | HINT, 1);
	mprotect_case("RW | PROT_MTE", rw | HINT2, 1);
	mprotect_case("RWX | PROT_BTI", rw | PROT_EXEC | HINT, 1);
	/* An unknown bit must still be refused, so that a typo is not silently
	 * swallowed along with the hints. */
	mprotect_case("RW | unknown 0x40000000", rw | 0x40000000, 0);
	mprotect_case("RW | BTI and MTE", rw | HINT | HINT2, 1);

	printf("PROT_HINT: %s\n", failures ? "FAIL" : "DONE");
	return failures ? 1 : 0;
}
