"""Final memfd release must break the dirty-page/vnode reference cycle."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools.tests.test_usb_hid_readiness import function
ROOT = Path(__file__).resolve().parents[2]
MODEL = r'''
#include <assert.h>
#include <stddef.h>
typedef struct {int refs, cache, released;} vnode_t;
typedef struct {vnode_t *vnode; void *priv;int refs;} vfile_t;
static void vnode_put(vnode_t *vn){assert(vn->refs>0);if(!--vn->refs)vn->released++;}
static void page_cache_discard_unlinked(vnode_t *vn){while(vn->cache){vn->cache--;vnode_put(vn);}}
'''
MAIN = r'''
static void put(vfile_t *vf){if(!--vf->refs){memfd_file_close(vf);vnode_put(vf->vnode);}}
int main(void){
 vnode_t vn={.refs=465,.cache=464};vfile_t vf={.vnode=&vn,.priv=&vn,.refs=4};
 /* Original fd, dup/SCM owner and two mappings share the open description. */
 put(&vf);put(&vf);assert(vf.priv&&vn.cache==464&&!vn.released);
 put(&vf);assert(vf.priv&&vn.cache==464&&!vn.released);
 put(&vf);assert(!vf.priv&&vn.cache==0&&vn.refs==0&&vn.released==1);
 return 0;
}
'''
class MemfdCacheLifetime(unittest.TestCase):
    def test_final_description_release_after_mappings(self):
        code=function((ROOT/'kernel/fs/memfd.c').read_text(),'memfd_file_close')
        with tempfile.TemporaryDirectory() as directory:
            p=Path(directory)
            def run(body,name):
                source,binary=p/(name+'.c'),p/name
                source.write_text(MODEL+body+MAIN)
                subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function',
                                '-fsanitize=undefined',str(source),'-o',str(binary)],check=True,capture_output=True)
                return subprocess.run([str(binary)],capture_output=True).returncode
            self.assertEqual(run(code,'fixed'),0)
            old=code.replace('        page_cache_discard_unlinked(vf->vnode);\n','')
            self.assertNotEqual(old,code)
            self.assertNotEqual(run(old,'old'),0)
if __name__=='__main__': unittest.main()
