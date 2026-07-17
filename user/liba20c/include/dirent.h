#ifndef _DIRENT_H
#define _DIRENT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dirent {
    unsigned long  d_ino;
    unsigned char  d_type;
    char           d_name[256];
};

typedef struct __dirstream DIR;

#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12

DIR *opendir(const char *name);
struct dirent *readdir(DIR *dirp);
int closedir(DIR *dirp);
void rewinddir(DIR *dirp);
long telldir(DIR *dirp);
void seekdir(DIR *dirp, long loc);

#ifdef __cplusplus
}
#endif

#endif
