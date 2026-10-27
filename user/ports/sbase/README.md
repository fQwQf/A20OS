# A20 id command

`id.c` is the existing A20 addition to sbase. The main user Makefile compiles it
into the sbase id object and links the usual libutil/libutf libraries. Keeping
this source here allows the sbase submodule to remain byte-identical to upstream.
The associated MIT license is preserved in LICENSE.
