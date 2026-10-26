"""Bay Trail TSC MSR discovery: production helper with synthetic CPU/MSR I/O."""
import ctypes
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class BayTrailTsc(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="a20-baytrail-tsc-")
        path = Path(cls.tmp.name)
        (path / "wrapper.c").write_text(r'''
#include "arch/x86_64/platform/tsc_baytrail.h"
static uint32_t max_leaf, signature, features, vendor;
static uint64_t platform_info, fsb_freq;
static unsigned msr_reads, cpuid_reads, bad_msr;
static void probe_cpuid(void *opaque, uint32_t leaf, uint32_t *a,
                        uint32_t *b, uint32_t *c, uint32_t *d) {
    (void)opaque; cpuid_reads++; *a = *b = *c = *d = 0;
    if (!leaf) {
        *a = max_leaf; *b = 0x756e6547U; *d = 0x49656e69U; *c = 0x6c65746eU;
        if (vendor == 1) *b ^= 1;
        if (vendor == 2) *c ^= 1;
        if (vendor == 3) *d ^= 1;
    } else if (leaf == 1) { *a = signature; *c = features; }
}
static uint64_t probe_msr(void *opaque, uint32_t msr) {
    (void)opaque; msr_reads++;
    if (msr == 0xce) return platform_info;
    if (msr == 0xcd) return fsb_freq;
    bad_msr++; return 0;
}
uint64_t probe(uint32_t limit, uint32_t sig, uint32_t caps, uint32_t other_vendor,
               uint64_t info, uint64_t fsb) {
    max_leaf = limit; signature = sig; features = caps; vendor = other_vendor;
    platform_info = info; fsb_freq = fsb; msr_reads = cpuid_reads = bad_msr = 0;
    const struct x86_tsc_probe_ops ops = {0, probe_cpuid, probe_msr};
    return x86_baytrail_tsc_hz(&ops);
}
unsigned reads(void) { return msr_reads; }
unsigned cpus(void) { return cpuid_reads; }
unsigned bad(void) { return bad_msr; }
''')
        subprocess.run([
            "cc", "-shared", "-fPIC", "-O2", "-Wall", "-Wextra", "-Werror",
            "-I", str(ROOT / "kernel"), "-I", str(ROOT / "kernel/include"),
            str(path / "wrapper.c"), "-o", str(path / "model.so"),
        ], check=True)
        cls.lib = ctypes.CDLL(str(path / "model.so"))
        cls.lib.probe.argtypes = [ctypes.c_uint32] * 4 + [ctypes.c_uint64] * 2
        cls.lib.probe.restype = ctypes.c_uint64

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def probe(self, *, limit=0xb, signature=0x30678, features=0, vendor=0,
              info=29 << 8, fsb=0):
        hz = self.lib.probe(limit, signature, features, vendor, info, fsb)
        self.assertEqual(self.lib.bad(), 0)
        return hz

    def test_j1800_fractional_reference(self):
        # J1800 nominal "2.41 GHz" is 29 * 83 1/3 MHz, not a rounded 2.4 GHz.
        self.assertEqual(self.probe(), 2416666667)
        self.assertEqual(self.lib.reads(), 2)

    def test_all_documented_bus_encodings_and_ratios(self):
        ratios = [(5, 6), (1, 1), (4, 3), (7, 6), (4, 5)]
        for index, (num, den) in enumerate(ratios):
            for multiplier in (1, 12, 24, 29, 31, 85, 255):
                with self.subTest(index=index, multiplier=multiplier):
                    expected = (100000000 * num * multiplier + den // 2) // den
                    if not 100000000 <= expected <= 10000000000:
                        expected = 0
                    self.assertEqual(self.probe(info=multiplier << 8, fsb=index), expected)

    def test_unrelated_msr_fields_are_ignored(self):
        self.assertEqual(self.probe(info=0xffffffffffff1dff, fsb=0xfffffffffffffff8),
                         2416666667)

    def test_reserved_bus_values_zero_ratio_and_excessive_rate(self):
        for index in (5, 6, 7):
            self.assertEqual(self.probe(fsb=index), 0)
        self.assertEqual(self.probe(info=0), 0)
        self.assertEqual(self.probe(info=255 << 8, fsb=1), 0)

    def test_wrong_vendors_never_read_model_specific_msrs(self):
        for vendor in (1, 2, 3):
            self.assertEqual(self.probe(vendor=vendor), 0)
            self.assertEqual(self.lib.reads(), 0)
            self.assertEqual(self.lib.cpus(), 1)

    def test_missing_cpuid_leaf_and_other_families_models_never_read_msrs(self):
        self.assertEqual(self.probe(limit=0), 0)
        self.assertEqual(self.lib.reads(), 0)
        # Atom Bonnell, Avoton, Cherry Trail, Core, family 15, extended family.
        for signature in (0x106c2, 0x406d8, 0x406c3, 0x306a9, 0x30f78, 0x130f78):
            self.assertEqual(self.probe(signature=signature), 0)
            self.assertEqual(self.lib.reads(), 0)

    def test_virtual_cpu_model_cannot_trigger_unimplemented_msrs(self):
        self.assertEqual(self.probe(features=1 << 31), 0)
        self.assertEqual(self.lib.reads(), 0)
        # Stepping does not change the family/model gate.
        for stepping in range(16):
            self.assertEqual(self.probe(signature=0x30670 | stepping), 2416666667)


if __name__ == "__main__":
    unittest.main()
