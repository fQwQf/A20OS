#ifndef A20_X86_HYP_SVM_H
#define A20_X86_HYP_SVM_H
/* AMD APM volume 2, publication 24593, chapters 15 and appendix B.
 * Byte offsets name hardware fields, independent of compiler struct packing. */
#define SVM_EFER 0xc0000080U
#define SVM_VM_CR 0xc0010114U
#define SVM_HSAVE_PA 0xc0010117U
#define SVM_SVME (1ULL << 12)
#define SVM_IOPM 0x040
#define SVM_MSRPM 0x048
#define SVM_ASID 0x058
#define SVM_TLB_CTL 0x05c
#define SVM_INT_CTL 0x060
#define SVM_EXIT_CODE 0x070
#define SVM_EXIT_INFO1 0x078
#define SVM_EXIT_INFO2 0x080
#define SVM_NESTED_CTL 0x090
#define SVM_NCR3 0x0b0
#define SVM_NEXT_RIP 0x0c8
#define SVM_ES 0x400
#define SVM_CS 0x410
#define SVM_SS 0x420
#define SVM_DS 0x430
#define SVM_FS 0x440
#define SVM_GS 0x450
#define SVM_GDTR 0x460
#define SVM_LDTR 0x470
#define SVM_IDTR 0x480
#define SVM_TR 0x490
#define SVM_GUEST_EFER 0x4d0
#define SVM_CR4 0x548
#define SVM_CR3 0x550
#define SVM_CR0 0x558
#define SVM_DR7 0x560
#define SVM_DR6 0x568
#define SVM_RFLAGS 0x570
#define SVM_RIP 0x578
#define SVM_RSP 0x5d8
#define SVM_RAX 0x5f8
#define SVM_STAR 0x600
#define SVM_LSTAR 0x608
#define SVM_CSTAR 0x610
#define SVM_SFMASK 0x618
#define SVM_KERNEL_GS 0x620
#define SVM_SYSENTER_CS 0x628
#define SVM_SYSENTER_ESP 0x630
#define SVM_SYSENTER_EIP 0x638
#define SVM_PAT 0x668
#endif
