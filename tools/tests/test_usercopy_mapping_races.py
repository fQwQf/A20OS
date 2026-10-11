"""Exercise real user-copy loops while a COW replacement is ready to run."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class UsercopyMappingRaces(unittest.TestCase):
    def test_copy_and_string_access_hold_the_mapping(self):
        source = (ROOT / 'kernel/mm/mm.c').read_text()
        body = source[source.index('static int user_access_begin('):]
        harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define PAGE_SIZE 4096
#define PAGE_OFFSET 0
#define USER_VA_LIMIT 0x10000
#define EFAULT 14
#define EINVAL 22
#define VM_READ 1
#define VM_WRITE 2
#define PTE_A 4
#define PTE_D 8
#define MM_FAULT_ACCESS_WRITE 1
#define MM_FAULT_ACCESS_READ 0
typedef uintptr_t pte_t;
typedef uintptr_t vaddr_t;
typedef struct { int lock; } mm_t;
typedef struct { unsigned vm_flags; } mm_seg_t;
typedef struct { mm_t *mm; pte_t *pgdir; } task_t;
static mm_t mm;
static pte_t leaf;
static task_t task={&mm,&leaf};
static mm_seg_t vma={VM_READ|VM_WRITE};
static unsigned char old_page[PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));
static unsigned char new_page[PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));
static unsigned pending, copied, faults;
static task_t *proc_current(void) { return &task; }
static int user_range_ok(uint64_t va,size_t n) {
    return !n || (va < USER_VA_LIMIT && n <= USER_VA_LIMIT-va);
}
static void replacement(void) {
    assert(!mm.lock);
    memcpy(new_page,old_page,PAGE_SIZE);
    leaf=(uintptr_t)new_page|3;
    memset(old_page,0xa5,PAGE_SIZE); /* retiring frame gets reused */
    pending=0;
}
static uint64_t spin_lock_irqsave(int *lock) { assert(!*lock); *lock=1; return 42; }
static void spin_unlock_irqrestore(int *lock,uint64_t flags) {
    assert(*lock && flags==42); *lock=0;
    if (pending && copied) replacement();
}
static pte_t *pt_lookup_leaf(pte_t *root,uint64_t va,void *level,vaddr_t *base,size_t *size) {
    (void)root; (void)level; assert(mm.lock && va>=0x1000 && va<0x2000);
    if(base)*base=0x1000;
    if(size)*size=PAGE_SIZE;
    return &leaf;
}
static int pte_user_writable(pte_t p) { return (p&3)==3; }
static int pte_user_readable(pte_t p) { return (p&1)!=0; }
static uintptr_t arch_pte_addr(pte_t p) { return p&~(uintptr_t)(PAGE_SIZE-1); }
static mm_seg_t *mm_seg_find(mm_t *m,uint64_t va) { (void)m;(void)va; assert(mm.lock); return &vma; }
static int handle_demand_fault_access(task_t *t,uint64_t va,int access) {
    (void)t;(void)va;(void)access;assert(!mm.lock);faults++;leaf=(uintptr_t)old_page|3;return 0;
}
static void *checked_copy(void *dst,const void *src,size_t n) {
    assert(mm.lock && n<=PAGE_SIZE);
    assert(!pending || (leaf&~(uintptr_t)4095)==(uintptr_t)old_page);
    copied=1;
    return memcpy(dst,src,n);
}
#define memcpy checked_copy
''' + body + r'''
#undef memcpy
static void reset(void) {
    assert(!mm.lock);memset(old_page,0,4096);memset(new_page,0,4096);
    leaf=(uintptr_t)old_page|3;vma.vm_flags=3;pending=copied=faults=0;
}
int main(void) {
    reset(); uint64_t value=0xabc123456789ULL;
    pending=1;
    assert(copy_to_user((void*)0x1000,&value,8)==8);
    assert(!pending && !mm.lock);
    assert(*(uint64_t*)new_page==value);
    assert((leaf&PTE_D)==0); /* replacement constructed a new leaf */
    reset();memcpy(old_page,&value,8);pending=1;uint64_t out=0;
    assert(copy_from_user(&out,(void*)0x1000,8)==8 && out==value && !mm.lock);
    reset();leaf=0;assert(copy_to_user((void*)0x1000,&value,8)==8);
    assert(faults==1 && !mm.lock && (leaf&PTE_D));
    reset();vma.vm_flags=VM_READ;
    assert(copy_to_user((void*)0x1000,&value,8)==-EFAULT && !mm.lock && !faults);
    reset();memcpy(old_page,"hello",6);char str[16];
    assert(user_strncpy(str,(void*)0x1000,sizeof(str))==5 && !strcmp(str,"hello") && !mm.lock);
    assert(user_strnlen((void*)0x1000,16)==5 && !mm.lock);
    assert(user_strncpy(str,(void*)0x1000,4)==3 && str[3]==0 && !mm.lock);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src, binary = Path(tmp) / 'probe.c', Path(tmp) / 'probe'
            src.write_text(harness)
            subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(src),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
