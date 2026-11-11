"""Compile production socket stat dispatch; valid anonymous sockets are not EBADF."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools.tests.test_usb_hid_readiness import function

ROOT = Path(__file__).resolve().parents[2]
MODEL = r'''
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <sys/stat.h>
typedef struct {uint64_t st_ino;uint32_t st_mode,st_uid,st_gid,st_nlink;uint64_t st_blksize;unsigned char other[64];} kstat_t;
typedef struct {uint32_t stat_uid,stat_gid;} net_socket_t;
typedef struct vfile {void *vnode;int *ops;void *priv;uint64_t identity;} vfile_t;
static int g_net_ops,char_ops,pipe_ops,other_ops;
static int vfs_vnode_stat(void*p,kstat_t*s){(void)p;(void)s;return 55;}
static int vfs_is_char_device_vfile(vfile_t*f){return f->ops==&char_ops;}
static int vfs_is_pipe_vfile(vfile_t*f){return f->ops==&pipe_ops;}
static void fill_char_kstat(kstat_t*s){memset(s,0,sizeof(*s));s->st_mode=S_IFCHR|0666;}
static void fill_pipe_kstat(kstat_t*s){memset(s,0,sizeof(*s));s->st_mode=S_IFIFO|0600;}
'''
MAIN = r'''
int main(void){
 net_socket_t sock={.stat_uid=1000,.stat_gid=1001};
 vfile_t file={.ops=&g_net_ops,.priv=&sock,.identity=9123};kstat_t st;
 assert(!vfs_vfile_stat(&file,&st));assert(S_ISSOCK(st.st_mode));
 assert(st.st_ino==9123&&st.st_uid==1000&&st.st_gid==1001&&st.st_nlink==1&&st.st_blksize==4096);
 vfile_t *alias=&file;kstat_t second;
 assert(!vfs_vfile_stat(alias,&second)&&second.st_ino==st.st_ino);
 vfile_t distinct=file;distinct.identity++;
 assert(!vfs_vfile_stat(&distinct,&second)&&second.st_ino!=st.st_ino);
 file.vnode=&file;assert(vfs_vfile_stat(&file,&st)==55);file.vnode=0;
 file.ops=&char_ops;assert(!vfs_vfile_stat(&file,&st)&&S_ISCHR(st.st_mode));
 file.ops=&pipe_ops;assert(!vfs_vfile_stat(&file,&st)&&S_ISFIFO(st.st_mode));
 file.ops=&other_ops;assert(vfs_vfile_stat(&file,&st)==-EBADF);
 file.ops=&g_net_ops;file.priv=0;assert(vfs_vfile_stat(&file,&st)==-EBADF);
 return 0;
}
'''

class SocketFstat(unittest.TestCase):
    def test_socket_owner_uses_creation_filesystem_credentials(self):
        code = function((ROOT / 'kernel/net/socket/socket_file.c').read_text(), 'a20_net_install_prepare')
        source = r'''
#include <stdint.h>
#include <assert.h>
#include <stddef.h>
#include <fcntl.h>
typedef struct {int value;} refcount_t;
typedef struct {uint32_t stat_uid,stat_gid;} net_socket_t;
typedef struct {int flags;refcount_t ref_count;void *ops,*priv;} vfile_t;
typedef struct {struct {int uid,gid,fsuid,fsgid;} cred;} task_t;
static task_t creator={.cred={10,11,20,21}};
static task_t *proc_current(void){return &creator;}
static vfile_t file;
static vfile_t *vfile_alloc(void){return &file;}
static int g_net_ops;
static void refcount_set(refcount_t *r,int n){r->value=n;}
'''
        source += code + r'''
int main(void){net_socket_t socket={0};
 assert(a20_net_install_prepare(&socket,O_CLOEXEC|O_RDWR)==&file);
 assert(socket.stat_uid==20&&socket.stat_gid==21);
 creator.cred.fsuid=30;creator.cred.fsgid=31;
 assert(socket.stat_uid==20&&socket.stat_gid==21);return 0;}
'''
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'owner.c'
            path.write_text(source)
            binary = path.with_suffix('')
            subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined',
                            str(path), '-o', str(binary)], check=True, capture_output=True)
            subprocess.run([str(binary)], check=True, capture_output=True)

    def test_anonymous_socket_and_alias_metadata(self):
        socket = (ROOT / 'kernel/net/socket/socket_file.c').read_text()
        dispatch = function((ROOT / 'kernel/fs/vfs_stat.c').read_text(), 'vfs_vfile_stat')
        funcs = function(socket, 'net_is_socket_vfile') + function(socket, 'net_socket_stat_vfile')
        with tempfile.TemporaryDirectory() as temp:
            temp = Path(temp)
            def run(body, name):
                source, binary = temp / (name + '.c'), temp / name
                source.write_text(MODEL + funcs + body + MAIN)
                subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                                '-fsanitize=undefined', str(source), '-o', str(binary)],
                               check=True, capture_output=True)
                return subprocess.run([str(binary)], capture_output=True).returncode
            self.assertEqual(run(dispatch, 'fixed'), 0)
            old = dispatch.replace('    if (net_is_socket_vfile(vf))\n        return net_socket_stat_vfile(vf, st);\n', '')
            self.assertNotEqual(old, dispatch)
            self.assertNotEqual(run(old, 'old'), 0)

if __name__ == '__main__':
    unittest.main()
