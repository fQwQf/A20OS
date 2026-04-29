#ifndef _ABI_LINUX_MMAN_H
#define _ABI_LINUX_MMAN_H

#define PROT_READ      1
#define PROT_WRITE     2
#define PROT_EXEC      4

#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_FIXED      0x10
#define MAP_ANONYMOUS  0x20
#define MAP_POPULATE   0x8000
#define MAP_STACK      0x20000
#define MAP_HUGETLB    0x40000
#define MAP_FIXED_NOREPLACE 0x100000

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

#endif /* _ABI_LINUX_MMAN_H */
