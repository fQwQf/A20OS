"""Compare the production retirement check with a full PTE scan."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class TableEmpty(unittest.TestCase):
    def test_sparse_dense_swap_and_geometry(self):
        text = (ROOT / "kernel/mm/mm.c").read_text()
        start = text.index("static int pt_entry_mapped(pte_t entry) {")
        end = text.index("\n}", text.index("static int pt_table_empty(pte_t *table, int level, int cleared_idx) {")) + 2
        harness = r'''
#include <assert.h>
#include <stdint.h>
typedef uint64_t pte_t;
#define PTE_V 1
#ifdef CONFIG_SWAP
static int pte_is_swap(pte_t p) { return (p&3)==2; }
#endif
static int arch_pt_level_entries(int level) { return level ? 512 : 256; }
''' + text[start:end] + r'''
static uint32_t seed=101;
static uint32_t next(void) { seed=seed*1664525+1013904223; return seed; }
static int reference(pte_t *table,int n) {
    for(int i=0;i<n;i++) {
        if(table[i]&PTE_V) return 0;
#ifdef CONFIG_SWAP
        if(pte_is_swap(table[i])) return 0;
#endif
    }
    return 1;
}
int main(void) {
    pte_t table[512]={0};
    for(int level=0;level<2;level++) {
        int n=arch_pt_level_entries(level);
        /* Every single surviving position, including swaps, and every hint. */
        for(int kind=1;kind<=2;kind++) for(int pos=0;pos<n;pos++) {
            table[pos]=kind;
            for(int hint=-1;hint<=n;hint++)
                assert(pt_table_empty(table,level,hint)==reference(table,n));
            table[pos]=0;
        }
        for(int trial=0;trial<10000;trial++) {
            for(int i=0;i<n;i++) table[i]=(next()%31==0) ? next()%4 : 0;
            int hint=next()%n; table[hint]=0;
            assert(pt_table_empty(table,level,hint)==reference(table,n));
        }
        for(int i=0;i<n;i++) table[i]=PTE_V;
        /* Forward and reverse removal must find the last live entry. */
        for(int i=0;i<n;i++) {
            table[i]=0; assert(pt_table_empty(table,level,i)==(i==n-1));
        }
        for(int i=0;i<n;i++) table[i]=PTE_V;
        for(int i=n-1;i>=0;i--) {
            table[i]=0; assert(pt_table_empty(table,level,i)==(i==0));
        }
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / "test.c", Path(tmp) / "test"
            src.write_text(harness)
            for flags in [[], ["-DCONFIG_SWAP=1"]]:
                subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror", *flags, str(src), "-o", str(exe)], check=True)
                subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
