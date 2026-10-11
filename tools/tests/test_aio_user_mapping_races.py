"""Run positional AIO while VFS replaces the submitting thread's user page."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class AioUserMappingRaces(unittest.TestCase):
    def test_bounce_io_preserves_mapping_and_file_position(self):
        text = (ROOT / 'kernel/fs/aio.c').read_text()
        body = text[text.index('static int64_t aio_do_io('):
                    text.index('static int64_t aio_do_fsync(')]
        harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#define PAGE_SIZE 4096
#define EINVAL 22
#define EBADF 9
#define ESPIPE 29
#define ENOMEM 12
#define EFAULT 14
#define SEEK_CUR 1
#define SEEK_SET 0
typedef struct vfile vfile_t;
struct ops { long (*lseek)(vfile_t*,long,int); };
struct vfile { struct ops *ops; int offset_lock; long offset; };
static unsigned char user_old[8192],user_new[8192],disk[8192];
static unsigned char *user_page=user_old;
static int refs, allocations, replace, copy_fault;
static long seek_file(vfile_t *f,long off,int mode) {
    assert(f->offset_lock);
    if(mode==SEEK_CUR)return f->offset;
    f->offset=off;return off;
}
static struct ops ops={seek_file};
static vfile_t file={&ops,0,17};
static int64_t fdtable_get_current(int fd) { return fd==7?7:-EBADF; }
static vfile_t *vfs_get_file_ref(int fd) { assert(fd==7);refs++;return &file; }
static void vfs_put_file_ref(int fd,vfile_t *f) { assert(fd==7&&f==&file);refs--; }
static void *kmalloc(size_t n) { allocations++;return malloc(n); }
static void kfree(void *p) { if(p)allocations--;free(p); }
static void mutex_lock(int *p) { assert(!*p);*p=1; }
static void mutex_unlock(int *p) { assert(*p);*p=0; }
static void replace_user_mapping(void) {
    if(!replace)return;
    memcpy(user_new,user_old,sizeof(user_old));user_page=user_new;
    memset(user_old,0xa5,sizeof(user_old));replace=0;
}
static long vfs_read_file(vfile_t *f,void *p,size_t n) {
    assert(f->offset_lock);
    replace_user_mapping(); /* device sleeps while another thread breaks COW */
    memcpy(p,disk+f->offset,n);f->offset+=n;return n;
}
static long vfs_write_file(vfile_t *f,const void *p,size_t n) {
    assert(f->offset_lock);replace_user_mapping();
    memcpy(disk+f->offset,p,n);f->offset+=n;return n;
}
static long copy_to_user(void *p,const void *src,size_t n) {
    assert(!file.offset_lock);if(copy_fault)return -EFAULT;
    memcpy(user_page+(uintptr_t)p-0x10000,src,n);return n;
}
static long copy_from_user(void *p,const void *src,size_t n) {
    assert(!file.offset_lock);if(copy_fault)return -EFAULT;
    memcpy(p,user_page+(uintptr_t)src-0x10000,n);return n;
}
''' + body + r'''
static void reset(void) {
    assert(!refs&&!allocations&&!file.offset_lock);
    file.offset=17;user_page=user_old;copy_fault=0;replace=1;
    for(unsigned i=0;i<8192;i++){disk[i]=i%251;user_old[i]=(i+17)%253;}
}
int main(void) {
    reset();assert(aio_do_pread(7,0x10000,8192,0)==8192);
    assert(user_page==user_new&&!memcmp(user_new,disk,8192)&&file.offset==17);
    reset();unsigned char expected[8192];memcpy(expected,user_old,8192);
    assert(aio_do_pwrite(7,0x10000,8192,0)==8192);
    assert(!memcmp(expected,disk,8192)&&file.offset==17);
    reset();copy_fault=1;assert(aio_do_pread(7,0x10000,8,0)==-EFAULT);
    assert(file.offset==17);
    reset();assert(aio_do_pwrite(7,0x10000,8,-1)==-EINVAL);
    assert(aio_do_pread(7,0x10000,0,0)==0);
    assert(!refs&&!allocations&&!file.offset_lock);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            source, binary = Path(tmp) / 'probe.c', Path(tmp) / 'probe'
            source.write_text(harness)
            subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(source),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
