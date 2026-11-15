"""Failed devfs opens must preserve the backend error and both references."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class DevfsOpenError(unittest.TestCase):
    def test_errno_and_vnode_cleanup_with_missing_put_negative(self):
        text = (ROOT / "kernel/fs/devfs/devfs.c").read_text()
        begin = text.index("static vfile_t *devfs_open_failed(")
        end = text.index("static vfile_t *devfs_open_vnode(", begin)
        function = text[begin:end]
        prefix = r'''
#include <assert.h>
#include <stddef.h>
#include <errno.h>
typedef struct { int refs; } vnode_t;
typedef struct { vnode_t *vnode; int freed; } vfile_t;
typedef struct { int vfs_open_errno; } task_t;
static task_t current, *active=&current;
static task_t *proc_current(void) { return active; }
static void vnode_put(vnode_t *v) { assert(v->refs>0);v->refs--; }
static void vfile_free(vfile_t *v) { assert(!v->freed);v->freed=1; }
'''
        main = r'''
int main(void) {
    int errors[]={-EIO,-ENOSPC,-ENOMEM};
    for(int i=0;i<1000;i++) {
        vnode_t node={1};vfile_t file={&node,0};current.vfs_open_errno=0;
        assert(devfs_open_failed(&file,errors[i%3])==NULL);
        assert(current.vfs_open_errno==errors[i%3]);
        assert(node.refs==0 && file.freed);
    }
    active=NULL;
    vnode_t node={1};vfile_t file={&node,0};
    assert(devfs_open_failed(&file,-EIO)==NULL);
    assert(node.refs==0 && file.freed);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="a20-devfs-open-") as tmp:
            work = Path(tmp)
            for negative in (False, True):
                code = function
                if negative:
                    self.assertIn("vnode_put(vf->vnode);", code)
                    code = code.replace("vnode_put(vf->vnode);", "/* missing vnode put */")
                source, binary = work / "test.c", work / "test"
                source.write_text(prefix + code + main)
                subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra",
                                "-Wno-unused-function", "-Werror", str(source),
                                "-o", str(binary)], check=True)
                result = subprocess.run([str(binary)], capture_output=True)
                if negative:
                    self.assertNotEqual(result.returncode, 0)
                else:
                    self.assertEqual(result.returncode, 0, result.stderr.decode())


if __name__ == "__main__":
    unittest.main()
