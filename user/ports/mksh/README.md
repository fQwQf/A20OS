# mksh build inputs

These configuration/generated headers were previously stored inside the vendored
mksh tree. They are kept here so the submodule can use the unmodified upstream
commit. Both the musl and mlibc build rules add this directory to the include path.
The generated headers retain their original copyright and permission notices.

Regenerate/review these files when updating mksh. The upstream source retains
normal script descriptor duplication and close-on-exec behavior.
