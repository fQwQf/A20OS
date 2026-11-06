"""Differential check of real legacy-range clipping, including error propagation."""
from pathlib import Path
import subprocess,tempfile
from test_pt_cache_owner import extract
ROOT=Path(__file__).resolve().parents[2]
s=extract((ROOT/'kernel/mm/vm.c').read_text(),'static int mm_fork_clone_uncovered(')
h=r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
typedef uintptr_t vaddr_t;
typedef struct mm_seg {uintptr_t start,end;struct mm_seg *next;} mm_seg_t;
typedef struct {mm_seg_t *mmap;} mm_struct_t;
static unsigned char visited[256];static int calls,fail;
static int mm_fork_clone_range(mm_struct_t *c,mm_struct_t *p,uintptr_t a,uintptr_t b,int shared){(void)c;(void)p;assert(!shared&&a<b&&b<=256);calls++;if(fail&&calls==fail)return -12;for(;a<b;a++){assert(!visited[a]);visited[a]=1;}return 0;}
/* REAL */
static unsigned rng=13579;
static unsigned next(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
int main(void){
 for(int trial=0;trial<20000;trial++){
  mm_seg_t v[64];unsigned char covered[256]={0};int count=0;
  for(unsigned a=0;a<250&&count<64;){a+=next()%8;if(a>=250)break;unsigned b=a+1+next()%6;if(b>256)b=256;v[count]=(mm_seg_t){a,b,NULL};if(count)v[count-1].next=&v[count];count++;for(unsigned i=a;i<b;i++)covered[i]=1;a=b;}
  mm_struct_t parent={count?v:NULL},child={0};unsigned a=next()%257,b=next()%257;
  memset(visited,0,sizeof(visited));calls=fail=0;assert(!mm_fork_clone_uncovered(&child,&parent,a,b));
  for(unsigned i=0;i<256;i++)assert(visited[i]==(i>=a&&i<b&&!covered[i]));
  int n=calls;for(fail=1;fail<=n;fail++){memset(visited,0,sizeof(visited));calls=0;assert(mm_fork_clone_uncovered(&child,&parent,a,b)==-12);assert(calls==fail);}
 }
 puts("FORK_UNCOVERED: PASS 20000 sorted VMA maps, holes/boundaries/errors");return 0;
}
'''.replace('/* REAL */',s)
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.c';p.write_text(h);exe=Path(d)/'test'
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
