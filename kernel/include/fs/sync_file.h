#ifndef _FS_SYNC_FILE_H
#define _FS_SYNC_FILE_H

/* Export a fence after a synchronous device operation has completed. */
int sync_file_create_completed(const char *name);
/* Validate a completed sync_file supplied as an input dependency. */
int sync_file_wait_completed(int fd);

#endif
