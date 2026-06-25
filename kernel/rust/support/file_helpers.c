#include "core/fcntl.h"
#include "core/lock.h"
#include "core/sync.h"
#include "fs/vfs.h"
#include "mm/objcache.h"

static obj_cache_t g_vfile_cache;

void a20_file_table_init(void)
{
    obj_cache_init(&g_vfile_cache, "vfile", sizeof(vfile_t), 256);
}

vfile_t *a20_file_vfile_alloc(void)
{
    vfile_t *vf = obj_cache_alloc_zero(&g_vfile_cache);
    if (vf) {
        mutex_init(&vf->offset_lock);
        vf->lease = F_UNLCK;
    }
    return vf;
}

void a20_file_vfile_free(vfile_t *vf)
{
    obj_cache_free(&g_vfile_cache, vf);
}

void a20_file_vfile_ref_init(vfile_t *vf, int refs)
{
    if (vf)
        refcount_set(&vf->ref_count, refs);
}

void a20_file_vfile_get(vfile_t *vf)
{
    if (vf)
        refcount_inc(&vf->ref_count);
}

int a20_file_vfile_ref_read(vfile_t *vf)
{
    if (!vf)
        return 0;
    return refcount_read(&vf->ref_count);
}

int a20_file_vfile_put_ref_only(vfile_t *vf)
{
    if (!vf)
        return 0;
    return refcount_dec_and_test(&vf->ref_count);
}
