"""The guest reads the retained diagnostic for exactly its injected victim."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools.tests.test_usb_hid_readiness import function

ROOT = Path(__file__).resolve().parents[2]


class TrapStormRecord(unittest.TestCase):
    def test_retained_record_requires_pid_cause_address_and_exact_count(self):
        source = (ROOT / 'user/cmds/core/trap_storm_test.c').read_text()
        body = function(source, 'report_storm_record')
        harness = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
static const char *record;
static int capacity = 1024;
static int klogctl(int action, char *out, int size) {
    if (action == 10) return capacity;
    assert(action == 3 && size == 1024);
    memcpy(out, record, strlen(record));
    return strlen(record);
}
''' + body + r'''
int main(void) {
    record = "[ERR] [TRAP-STORM] pid=42 pc=0x102ac va=0x20000000 cause=15 count=4096\n";
    assert(report_storm_record(42) == 0);
    assert(report_storm_record(4) == -1);
    record = "[TRAP-STORM] pid=42 pc=0x102ac va=0x20000000 cause=15 count=40960\n";
    assert(report_storm_record(42) == -1);
    record = "[TRAP-STORM] pid=42 pc=0x102ac va=0x20001000 cause=15 count=4096\n";
    assert(report_storm_record(42) == -1);
    record = "[TRAP-STORM] pid=42 pc=0x102ac\nva=0x20000000 cause=15 count=4096\n";
    assert(report_storm_record(42) == -1);
    record = "";
    assert(report_storm_record(42) == -1);
    capacity = -1;
    assert(report_storm_record(42) == -1);
}
'''
        with tempfile.TemporaryDirectory(prefix='a20-storm-record-') as directory:
            path = Path(directory)
            (path / 'main.c').write_text(harness)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=undefined', str(path / 'main.c'),
                            '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()
