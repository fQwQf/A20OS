#ifndef _EXT4_JOURNAL_H
#define _EXT4_JOURNAL_H

#include "core/types.h"

typedef struct ext4_sb_info ext4_sb_info_t;
typedef struct ext4_journal ext4_journal_t;

/*
 * ext4's internal JBD2 log: recovery (read-only replay) and writing.
 *
 * A20OS runs the journal in ordered mode.  File data is not copied into the
 * log; it is forced to its home location first, and the metadata that
 * references it is only allowed to follow once a commit block makes the log
 * self-sufficient.  The one invariant every path depends on is expressed in
 * the block cache rather than in ext4: a metadata page covered by an
 * uncommitted transaction is *held*, so no generic sync -- not a timer, not
 * fsync, not unmount -- can write it home early.
 *
 * The crash-injection knob (a20.journal_crash=<point>) exists so the ordering
 * can be tested by stopping the machine at a chosen point instead of argued
 * about.  See tools/smoke_cases.py:smoke-journal-ext4.
 */

/* Open the journal for writing, after recovery has run.  A filesystem whose
 * superblock has no journal simply gets no writer. */
int  ext4_journal_open(ext4_sb_info_t *fs, const void *disk_sb);
void ext4_journal_close(ext4_sb_info_t *fs);

/* Record that the metadata covering [byte_off, byte_off + len) has been
 * written to the block cache, and pin those cache pages until the next
 * commit.  This is the write path every metadata site must use: the pin and
 * the record happen under the journal lock together with the cache write, so
 * a concurrent commit can never checkpoint a block whose transaction has not
 * been logged yet.  Falls through to a plain bcache write when the filesystem
 * has no journal. */
int  ext4_journal_meta_write(ext4_sb_info_t *fs, uint64_t byte_off,
                             const void *buf, size_t len);

/* Write, commit and checkpoint the pending transaction.  Idempotent: a commit
 * with nothing pending succeeds without touching the disk. */
int  ext4_journal_commit(ext4_sb_info_t *fs);

/* bcache pre-sync hook; install with bcache_set_sync_hook() at mount. */
int  ext4_journal_sync_hook(struct bcache *bc);

int  ext4_journal_pending(const ext4_sb_info_t *fs);

/* One line of state for the journal diagnostics; never fails. */
void ext4_journal_report(ext4_sb_info_t *fs);

#endif /* _EXT4_JOURNAL_H */