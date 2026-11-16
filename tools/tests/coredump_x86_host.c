/* Real x86 export + C adapter + Rust notes, consumed using host Linux headers. */
#include <assert.h>
#include <elf.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/procfs.h>
#include <sys/user.h>
#include "kernel/proc/coredump_layout.h"
#include "kernel/include/proc/debug_regs.h"
typedef struct {
    int pid,ppid,pgid,sid;
    struct {uint32_t uid,gid;} cred;
    char name[64];
} task_t;
/* TRAP_CONTEXT */
/* ARCH_EXPORT */
#define ARCH_COREDUMP_EXPORT_REGS arch_coredump_export_regs
/* NOTE_ADAPTER */
void rust_eh_personality(void) { abort(); } /* Host core.a linkage; never unwind. */
int main(int argc,char **argv) {
    assert(argc==2);
    _Static_assert(sizeof(struct elf_prstatus)==336,"Linux x86_64 PRSTATUS");
    _Static_assert(sizeof(struct elf_prpsinfo)==136,"Linux x86_64 PRPSINFO");
    trap_context_t ctx={0};
    uint64_t *words=(uint64_t *)&ctx;
    for(unsigned i=0;i<sizeof(ctx)/sizeof(*words);i++)words[i]=0x1000+i;
    task_t task={.pid=448,.ppid=420,.pgid=416,.sid=416,.cred={1000,1001},.name="xfsettingsd"};
    coredump_note_input_t input;
    coredump_fill_note_input(&task,11,&ctx,&input);
    char notes[2048];uint64_t length=0;
    assert(!a20_coredump_notes(&input,EM_X86_64,notes,sizeof(notes),&length));
    Elf64_Nhdr nh;memcpy(&nh,notes,sizeof(nh));
    assert(nh.n_namesz==5 && nh.n_descsz==sizeof(struct elf_prstatus) && nh.n_type==NT_PRSTATUS);
    assert(!memcmp(notes+sizeof(nh),"CORE",5));
    struct elf_prstatus status;memcpy(&status,notes+20,sizeof(status));
    assert(status.pr_pid==448 && status.pr_ppid==420 && status.pr_cursig==11);
    assert(!status.pr_fpvalid);
    struct user_regs_struct regs;memcpy(&regs,status.pr_reg,sizeof(regs));
    assert(regs.rax==ctx.rax && regs.rbx==ctx.rbx && regs.rcx==ctx.rcx && regs.rdx==ctx.rdx);
    assert(regs.rsi==ctx.rsi && regs.rdi==ctx.rdi && regs.rbp==ctx.rbp && regs.rsp==ctx.rsp);
    assert(regs.r8==ctx.r8 && regs.r9==ctx.r9 && regs.r10==ctx.r10 && regs.r11==ctx.r11);
    assert(regs.r12==ctx.r12 && regs.r13==ctx.r13 && regs.r14==ctx.r14 && regs.r15==ctx.r15);
    assert(regs.rip==ctx.rip && regs.eflags==ctx.rflags && regs.cs==ctx.cs && regs.ss==ctx.ss);
    assert(regs.fs_base==ctx.user_fs_base && regs.gs_base==ctx.user_gs_base);
    size_t next=20+sizeof(status);memcpy(&nh,notes+next,sizeof(nh));
    assert(nh.n_type==NT_PRPSINFO && nh.n_descsz==sizeof(struct elf_prpsinfo));
    struct elf_prpsinfo info;memcpy(&info,notes+next+20,sizeof(info));
    assert(info.pr_pid==448 && info.pr_uid==1000 && info.pr_gid==1001);
    assert(!strcmp(info.pr_fname,"xfsettingsd") && !strcmp(info.pr_psargs,"xfsettingsd"));
    assert(length==next+20+sizeof(info)); /* No fabricated x87/SSE descriptor. */
    uint64_t unchanged=0x1234;
    assert(a20_coredump_notes(&input,EM_X86_64,notes,16,&unchanged)==-12 && unchanged==0x1234);
    assert(!a20_coredump_notes(&input,EM_X86_64,notes,sizeof(notes),&length));
    char ehdr[64],phdr[56];coredump_layout_result_t layout;
    assert(!a20_coredump_layout(NULL,0,length,UINT64_MAX,4096,EM_X86_64,
        ehdr,phdr,sizeof(phdr),NULL,0,&layout));
    FILE *core=fopen(argv[1],"wb");assert(core);
    assert(fwrite(ehdr,1,sizeof(ehdr),core)==sizeof(ehdr));
    assert(fwrite(phdr,1,sizeof(phdr),core)==sizeof(phdr));
    assert(!fseek(core,(long)layout.notes_off,SEEK_SET));
    assert(fwrite(notes,1,length,core)==length && !fclose(core));
    puts("CORE_X86: production adapter/serializer agrees with Linux headers");
    return 0;
}
