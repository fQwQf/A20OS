#ifndef _CORE_MMAN_H
#define _CORE_MMAN_H

/*
 * Kernel-internal mman.h constant namespace.
 * Values intentionally match the Linux ABI wire format; the ABI layer
 * re-exports them (abi/linux/...).  Internal code must include this
 * header, never anything under abi/.
 */
#define PROT_NONE      0
#define PROT_READ      1
#define PROT_WRITE     2
#define PROT_EXEC      4

/*
 * AArch64 pointer-signature hints.  Both name page properties rather than
 * access, so neither may be OR'd into the R|W|X triple above: a mapping is
 * R+W+X-or-not independently of whether it was marked BTI-checked or
 * tag-checked.  The values match the Linux ABI wire format and musl's
 * arch/aarch64/bits/mman.h, so a program that includes <sys/mman.h> and passes
 * them compiles and links without a private copy of the header.
 *
 * Rejecting them as unknown -- which is what a bare
 * `prot & ~(PROT_READ|PROT_WRITE|PROT_EXEC)` test does -- turns such a caller
 * into an EINVAL it cannot explain, and it is a caller with no way to recover:
 * the bits arrive in the same argument as the access rights, so there is no
 * separate request to retry without them.  Accepting them means the hint is
 * dropped rather than honoured, which is a weaker guarantee and never a
 * stronger one: nothing here grants access a mapping would not otherwise get.
 */
#define PROT_BTI       0x10
#define PROT_MTE       0x20

/* Bits that select a page property rather than an access.  Stripped from
 * prot before it reaches the W^X policy and the page table, so a hint can
 * never be mistaken for a permission by either. */
#define PROT_HINT_MASK (PROT_BTI | PROT_MTE)

/* Every prot bit this kernel recognises: the three access bits plus the
 * accepted hints.  Anything else is still EINVAL. */
#define PROT_KNOWN_MASK (PROT_READ | PROT_WRITE | PROT_EXEC | PROT_HINT_MASK)

#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_FIXED      0x10
#define MAP_ANONYMOUS  0x20
#define MAP_POPULATE   0x8000
#define MAP_STACK      0x20000
#define MAP_HUGETLB    0x40000
#define MAP_FIXED_NOREPLACE 0x100000

#define MCL_CURRENT    1
#define MCL_FUTURE     2
#define MCL_ONFAULT    4

#define MREMAP_MAYMOVE    1
#define MREMAP_FIXED      2
#define MREMAP_DONTUNMAP  4

#define MADV_NORMAL      0
#define MADV_RANDOM      1
#define MADV_SEQUENTIAL  2
#define MADV_WILLNEED    3
#define MADV_DONTNEED    4
#define MADV_FREE        8
#define MADV_REMOVE      9
#define MADV_DONTFORK    10
#define MADV_DOFORK      11
#define MADV_MERGEABLE   12
#define MADV_UNMERGEABLE 13
#define MADV_HUGEPAGE    14
#define MADV_NOHUGEPAGE  15
#define MADV_DONTDUMP    16
#define MADV_DODUMP      17
#define MADV_WIPEONFORK  18
#define MADV_KEEPONFORK  19
#define MADV_COLD        20
#define MADV_PAGEOUT     21
#define MADV_POPULATE_READ 22
#define MADV_POPULATE_WRITE 23

#endif
