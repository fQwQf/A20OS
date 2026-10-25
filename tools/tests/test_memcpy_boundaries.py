"""Check production memcpy's vector cutovers without compiler builtin substitution."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class MemcpyBoundariesTest(unittest.TestCase):
    def test_all_alignments_and_transfer_boundaries(self):
        text = (ROOT / "kernel/core/string.c").read_text()
        body = text[text.index("void *memcpy("):text.index("void *memmove(")]
        body = body.replace("void *memcpy(", "void *production_memcpy(", 1)
        harness = r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
static unsigned char source[1100] __attribute__((aligned(16)));
static unsigned char destination[1100] __attribute__((aligned(16)));
int main(void) {
 for (unsigned s=0;s<16;s++) for(unsigned d=0;d<16;d++)
  for(unsigned n=0;n<=1024;n++) {
   for(unsigned i=0;i<1100;i++) {source[i]=(unsigned char)(i*37+11);destination[i]=0xa5;}
   assert(production_memcpy(destination+d,source+s,n)==destination+d);
   for(unsigned i=0;i<1100;i++)
    assert(destination[i]==(i>=d&&i<d+n?source[s+i-d]:0xa5));
  }
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="a20-memcpy-") as directory:
            work = Path(directory)
            source = work / "test.c"
            source.write_text("#include <stdint.h>\n#include <stddef.h>\n" + body + harness)
            executable = work / "test"
            subprocess.run(["cc", "-std=gnu99", "-O2", "-fno-builtin",
                            "-fsanitize=undefined", "-fno-sanitize-recover=all",
                            str(source), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
