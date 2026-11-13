"""Exercise the production per-device evdev multitouch state."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class MultitouchStateTest(unittest.TestCase):
    def test_slots_resync_and_release(self):
        source = r'''
#include <assert.h>
#include "kernel/drivers/input/mt_state.h"
int main(void) {
 struct input_mt_state a, b;
 int ids[16], x[16];
 input_mt_init(&a); input_mt_init(&b);
 assert(!input_mt_snapshot(&a, 0x39, ids, 16));
 for(int i=0;i<16;i++) assert(ids[i]==-1);
 input_mt_update(&a,0x2f,0);
 input_mt_update(&a,0x39,12);
 input_mt_update(&a,0x35,100);
 input_mt_update(&a,0x2f,4);
 input_mt_update(&a,0x39,31);
 input_mt_update(&a,0x35,800);
 assert(!input_mt_snapshot(&a,0x39,ids,5));
 assert(!input_mt_snapshot(&a,0x35,x,5));
 assert(ids[0]==12 && ids[4]==31 && x[0]==100 && x[4]==800);
 input_mt_update(&a,0x39,-1);
 assert(!input_mt_snapshot(&a,0x39,ids,5));
 assert(ids[0]==12 && ids[4]==-1);
 assert(!input_mt_snapshot(&b,0x39,ids,5));
 for(int i=0;i<5;i++) assert(ids[i]==-1);
 assert(input_mt_snapshot(&a,0x2f,ids,5)==-1);
 assert(input_mt_snapshot(&a,0x39,ids,17)==-1);
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="a20-mt-") as temporary:
            path = Path(temporary)
            (path / "test.c").write_text(source)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT), str(path/"test.c"), "-o", str(path/"test")],
                           check=True, capture_output=True)
            subprocess.run([str(path/"test")], check=True, timeout=5)


if __name__ == "__main__":
    unittest.main()
