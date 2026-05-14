# Page Cache Plan

This document describes the long-term A20OS page cache direction.

## Current State

`kernel/fs/block_cache.c` already contains two caches:

- a 512-byte block cache keyed by device LBA;
- an internal 4096-byte `pcache_*` layer keyed by device page number.

That `pcache_*` layer is useful for reducing small block I/O, but it is not a
VFS page cache. It does not know which file or vnode owns a page, so it cannot
be shared by `read`, `mmap`, and `exec` at the file-object level.

The initial VFS page cache lives in:

- `kernel/include/fs/page_cache.h`
- `kernel/fs/page_cache.c`

It is keyed by `(vnode_t *, page_index)` and owns a page-sized buffer per cache
entry. Entries hold a vnode reference while valid.

## Design Goals

1. One file page identity: `(vnode, page_index)`.
2. `read`, file-backed `mmap`, and ELF loading should eventually see the same
   cached file page.
3. Dirty page state belongs to the page cache, not ad hoc filesystem buffers.
4. Filesystem code supplies fill and writeback operations; page cache code owns
   lookup, pinning, invalidation, truncation, and LRU policy.
5. The block cache remains a lower-level device cache. Filesystems may use it to
   fill or write back page-cache pages.

## Staged Integration

Stage 1 is complete: add the generic page-cache object model and initialize it
from VFS without changing read/write behavior.

Stage 2 should add filesystem fill/writeback adapters:

- FAT32: map file offset to cluster chain, fill a page through block cache.
- EXT4: map logical file block to physical block, fill a page through block cache.
- RAMFS/MEMFD: either bypass page cache or use it as the backing store.

Stage 3 should route regular-file `vfs_read_file()` through page cache for
cacheable vnode types, while keeping existing file ops as fallback.

Stage 4 should make file-backed `mmap` fault pages from page cache instead of
allocating anonymous pages and copying file data.

Stage 5 should make ELF segment loading consume page-cache pages. Executable
private mappings can still map COW/private frames initially; the cache is the
source of truth for file bytes.

## Non-Goals For The First Cut

- No writeback daemon.
- No memory pressure shrinker.
- No direct mapping of page-cache frames into userspace.
- No unified page/buffer cache replacement for all block-cache users.

These are later steps once locking, wait queues, and filesystem writeback rules
are firmer.
