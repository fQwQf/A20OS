#!/bin/sh
# Exercise sbase coreutils built on mlibc (Native ABI), launched from the
# mksh (Linux ABI) shell.  Prints MLIBC_SBASE: PASS on success, or
# FAIL <name> with the expected/got values otherwise.
#
# Note: the FAT32 filesystem is mounted at /bin, so executables live at
# /bin/mlibc-*.  File-system tests run under the writable ramfs root (/a20test)
# because native absolute path resolution does not traverse the /bin mount.
fail=0
check() { # $1 name  $2 expected  $3 actual
	[ "$2" = "$3" ] || { echo "FAIL $1 (expected='$2' got='$3')"; fail=1; }
}

check echo "$( /bin/mlibc-echo hello world )" "hello world"
check true "$( /bin/mlibc-true && echo ok )" "ok"
check false "$( /bin/mlibc-false || echo ok )" "ok"
check basename "$( /bin/mlibc-basename /a/b/c.txt )" "c.txt"
check dirname "$( /bin/mlibc-dirname /a/b/c.txt )" "/a/b"
check seq "$( /bin/mlibc-seq 1 3 )" "$(printf '1\n2\n3')"
check seq-cat "$( /bin/mlibc-seq 1 4 | /bin/mlibc-cat )" "$(printf '1\n2\n3\n4')"

# Native-Native pipe through the shell: printf writes to mksh pipe (Linux
# ABI), head/tail are Native ABI readers.
check head "$( printf 'a\nb\nc\n' | /bin/mlibc-head -n 2 )" "$(printf 'a\nb')"
check tail "$( printf 'a\nb\nc\n' | /bin/mlibc-tail -n 1 )" "c"

# Filesystem round-trip: mksh creates the file, mlibc echo writes to the
# inherited fd, mlibc cat opens/reads it back.
D=/a20test
/bin/mlibc-mkdir "$D" >/dev/null 2>&1
check file-write "$( /bin/mlibc-echo -n hello > "$D/f" && /bin/mlibc-cat "$D/f" )" "hello"

/bin/mlibc-mkdir "$D/sub" >/dev/null 2>&1 && [ -d "$D/sub" ] || { echo "FAIL mkdir"; fail=1; }
/bin/mlibc-ln "$D/f" "$D/link" >/dev/null 2>&1 && [ -f "$D/link" ] || { echo "FAIL ln"; fail=1; }
/bin/mlibc-cp "$D/f" "$D/copied" >/dev/null 2>&1 && [ -f "$D/copied" ] || { echo "FAIL cp"; fail=1; }
/bin/mlibc-rm -f "$D/copied" >/dev/null 2>&1 && [ ! -e "$D/copied" ] || { echo "FAIL rm"; fail=1; }

# cleanup
/bin/mlibc-rm -f "$D/link" "$D/f" >/dev/null 2>&1
/bin/mlibc-rmdir "$D/sub" >/dev/null 2>&1
/bin/mlibc-rmdir "$D" >/dev/null 2>&1

check sleep "$( /bin/mlibc-sleep 0; echo ok )" "ok"
check sync "$( /bin/mlibc-sync; echo ok )" "ok"
[ -n "$( /bin/mlibc-uname -s )" ] || { echo "FAIL uname"; fail=1; }
[ -n "$( /bin/mlibc-hostname )" ] || { echo "FAIL hostname"; fail=1; }
check yes "$( /bin/mlibc-yes | /bin/mlibc-head -n 1 )" "y"

[ $fail -eq 0 ] && echo "MLIBC_SBASE: PASS" || echo "MLIBC_SBASE: FAIL"
