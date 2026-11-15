"""Both VMM runners must normalize a peer's shutdown before terminal CAS."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools.tests.test_pt_mcs_preempt_window import extract_function

ROOT = Path(__file__).resolve().parents[2]


class HypGroupExit(unittest.TestCase):
    def test_shutdown_stop_and_error_remain_distinct(self):
        source = (ROOT / 'user/cmds/core/hypvm.c').read_text()
        body = extract_function(source, 'static void stop_group(struct vmm_group *group, int reason, int error)')
        fixture = r'''
#include <assert.h>
#include <errno.h>
#define HYP_GUEST_EXIT_SHUTDOWN 1
#define HYP_GUEST_EXIT_ERROR 3
#define HYP_GUEST_EXIT_STOPPED 5
struct vmm_group { long vm, vcpus[2]; unsigned count; int done, terminal, error; };
struct hyp_guest_status { long exit; };
static int result, controls;
static long status_exit;
static int hyp_guest_vm_status(long vm, struct hyp_guest_status *status) {
    assert(vm==42); status->exit=status_exit; errno=EIO; return result;
}
static int hyp_guest_vcpu_control(long v, unsigned command) {
    assert((v==3||v==4)&&command==3); controls++; return 0;
}
''' + body + r'''
int main(void) {
    for(int i=0;i<4;i++) {
        struct vmm_group group={.vm=42,.vcpus={3,4},.count=2};
        result=i==3 ? -1 : 0;
        status_exit=i==0 ? 1 : i==1 ? 5 : 3;
        controls=0;
        stop_group(&group,5,0);
        assert(group.done && controls==2);
        assert(group.terminal==(i==0 ? 1 : i==1 ? 5 : 3));
        assert(group.error==(i>=2 ? EIO : 0));
        /* A later explicit stop must not overwrite the terminal winner. */
        result=0; status_exit=5;
        stop_group(&group,5,0);
        assert(group.terminal==(i==0 ? 1 : i==1 ? 5 : 3));
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='a20-hyp-terminal-') as d:
            p = Path(d)
            (p/'main.c').write_text(fixture)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'main.c'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)


if __name__ == '__main__':
    unittest.main()
