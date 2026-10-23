#ifndef _ELF_RUST_H
#define _ELF_RUST_H

/*
 * C bridge to the Rust ELF/exec decision core (kernel/mm/elf_core.rs).
 *
 * This header is the single C spelling of the #[repr(C)] ABI the Rust crate
 * exports. The Rust side pins every size in a const-assert block
 * (elf_core.rs, "Pin the FFI layout"); the _Static_asserts below pin the C
 * side, so the two cannot silently disagree. The Rust ELF/exec decision
 * core is the only implementation; this header is included unconditionally
 * by the C adapter layer (kernel/mm/elf.c, kernel/proc/exec.c,
 * kernel/ipc/kexec.c).
 *
 * Ownership: every decision (validation, layout, W^X, stack/auxv planning,
 * creds, reloc policy, interp ordering, kexec bounds, commit/rollback order)
 * lives in Rust. The C side keeps only the reviewed adapter primitives:
 * file reads, page/frame mapping, direct-map copies, usercopy, locks and the
 * architecture register-state / final machine entry.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define A20_ELF_MAX_PHDRS          64
#define A20_ELF_MAX_ARG_STRINGS    256
#define A20_ELF_AUXV_MAX_PAIRS     32
#define A20_ELF_KEXEC_MAX_SEGMENTS 16

/* W^X policy values, mirroring a20.wx=off|strip|deny. */
#define A20_ELF_MM_WX_OFF    0
#define A20_ELF_MM_WX_STRIP  1
#define A20_ELF_MM_WX_DENY   2

/* Format-resolution step codes (a20_elf_classify / a20_elf_step_after_*). */
#define A20_ELF_STEP_INSTALL    0
#define A20_ELF_STEP_RETRY      1
#define A20_ELF_STEP_CONTINUE   2
#define A20_ELF_STEP_FAIL       3

/* Commit-phase discriminants (a20_elf_commit_phase_at / a20_elf_rollback_at). */
#define A20_ELF_PHASE_STACK_BUILD        0
#define A20_ELF_PHASE_VDSO_MAP           1
#define A20_ELF_PHASE_ALLOC_NEW_MM       2
#define A20_ELF_PHASE_PUBLISH_AUXV       3
#define A20_ELF_PHASE_FDTABLE_UNSHARE    4
#define A20_ELF_PHASE_TERMINATE_SIBLINGS 5
#define A20_ELF_PHASE_CLOSE_ON_EXEC      6
#define A20_ELF_PHASE_TRAP_CONTEXT       7
#define A20_ELF_PHASE_CREDS              8
#define A20_ELF_PHASE_CPU_ENTER          9

/* Relocation action verdicts (a20_elf_reloc_action). */
#define A20_ELF_RELOC_ACT_SKIP      0
#define A20_ELF_RELOC_ACT_RELATIVE  1
#define A20_ELF_RELOC_ACT_ABSOLUTE  2

/* Interpreter ladder classifications (a20_elf_interp_resolution). */
#define A20_ELF_INTERP_RESOLUTION_EXACT          0
#define A20_ELF_INTERP_RESOLUTION_MOUNT_RELATIVE 1
#define A20_ELF_INTERP_RESOLUTION_SIBLING        2
#define A20_ELF_INTERP_RESOLUTION_SIBLING_LIBC   3
#define A20_ELF_INTERP_RESOLUTION_ARCH_FALLBACK  4
#define A20_ELF_INTERP_RESOLUTION_NOT_FOUND     (-1)

/* ---- #[repr(C)] mirrors of the Rust plan structs ---- */

typedef struct {
    uint8_t  class;
    uint8_t  data;
    uint16_t e_type;
    uint16_t e_machine;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint16_t e_phentsize;
    uint16_t e_phnum;
} a20_elf_ehdr_t;

typedef struct {
    uint16_t ph_index;
    uint16_t _pad0;
    int32_t  prot;          /* filtered PROT_* the segment maps with */
    uint32_t p_flags;
    uint64_t va;
    uint64_t memsz;
    uint64_t filesz;
    uint64_t offset;
    uint64_t align;
    uint8_t  anon_tail;     /* p_memsz > p_filesz */
    uint8_t  _pad1[7];
} a20_elf_segment_plan_t;

typedef struct {
    uint8_t  present;
    uint8_t  _pad0[7];
    uint64_t offset;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
} a20_elf_tls_plan_t;

typedef struct {
    uint8_t  present;
    uint8_t  _pad0[7];
    uint64_t offset;
    uint64_t filesz;
} a20_elf_interp_plan_t;

typedef struct {
    uint8_t  present;
    uint8_t  _pad0[7];
    uint64_t vaddr;
    uint64_t memsz;
} a20_elf_dynamic_plan_t;

typedef struct {
    a20_elf_ehdr_t          ehdr;
    uint8_t                 is_dyn;
    uint8_t                 is_native_abi;
    uint32_t                nseg;
    a20_elf_segment_plan_t  segments[A20_ELF_MAX_PHDRS];
    a20_elf_tls_plan_t      tls;
    a20_elf_interp_plan_t   interp;
    a20_elf_dynamic_plan_t  dynamic;
} a20_elf_image_plan_t;

typedef struct {
    uint64_t base;
    uint64_t max_va;
    uint64_t brk_va;
    uint64_t head_va;
    uint64_t hdr_map_va;
    uint64_t phdr_va;
    uint64_t entry;
    uint64_t exec_entry;
    uint64_t load_size;
} a20_elf_layout_t;

typedef struct {
    uint64_t pgdir;
    uint64_t mm;
    uint64_t tls_buffer;
    uint64_t tls_filesz;
    uint64_t tls_memsz;
    uint64_t tls_align;
    uint64_t tls_offset;
    uint64_t tls_tcb_offset;
    uint64_t tls_total_size;
    uint64_t tls_pages;
    uint64_t tls_tcb_size;
    int32_t interp_fd;
    uint32_t _pad;
    uint64_t interp_entry;
    uint64_t interp_base;
    uint64_t mmap;
    uint64_t stack_top;
    uint64_t tls_va;
    uint64_t tls_tp;
} a20_elf_load_resources_t;

typedef struct {
    uint64_t entry, exec_entry, base, end_va, brk, phdr_va;
    uint32_t phnum, phentsize;
    uint64_t load_addr, load_size, pgdir, stack_top, tls_va, tls_size,
             tls_tp, interp_base, mmap;
    uint32_t is_native_abi;
} a20_elf_load_result_t;

typedef struct {
    void *context;
    a20_elf_load_resources_t *resources;
    int32_t (*begin)(void *, uint64_t, uint64_t, uint64_t *);
    int32_t (*map_segments)(void *, uint64_t);
    int32_t (*stage_tls)(void *);
    int32_t (*relocate)(void *, uint64_t, uint64_t);
    int32_t (*finish_layout)(void *, const a20_elf_layout_t *);
    int32_t (*read_interpreter)(void *, uint64_t, uint8_t *, size_t);
    int32_t (*open_interpreter)(void *, const uint8_t *, size_t, int32_t *);
    size_t path_capacity;
    uint8_t nommu;
    /* Pad to the next pointer field.  A literal 7 is the LP64 size: on ILP32
     * the pointer needs only 4-byte alignment, so an explicit 7 bytes pushed
     * every callback that follows four bytes past where the Rust mirror puts
     * it, and the transaction read garbage Option pointers and refused every
     * image with -EINVAL. */
    uint8_t _pad0[sizeof(void *) - sizeof(uint8_t)];
    int32_t (*load_interpreter)(void *, uint64_t);
    void (*close_interpreter)(void *);
    int32_t (*map_stack)(void *);
    int32_t (*install_tls)(void *);
    int32_t (*build_interpreter_path)(void *, uint32_t, const uint8_t *, uint8_t *, size_t);
    int32_t (*arch_interpreter_path)(void *, const uint8_t *, uint8_t *, size_t);
    int32_t (*brk_offset)(void *, uint64_t, uint64_t *);
    int32_t (*commit)(void *, const a20_elf_load_result_t *);
    void (*free_tls)(void *);
    void (*discard_vmas)(void *);
    void (*discard_nommu)(void *);
    void (*destroy_pgdir)(void *);
} a20_elf_load_transaction_ops_t;

/* The first callback after the two non-pointer fields; pinned because a pad
 * sized for the wrong pointer width shifts every later entry and the Rust side
 * sees NULLs where callbacks should be. */
_Static_assert(__builtin_offsetof(a20_elf_load_transaction_ops_t,
                                  load_interpreter) ==
                   (sizeof(void *) == 8 ? 88 : 44),
               "elf_rust load-transaction ops layout");

typedef struct {
    void *context;
    int32_t (*read)(void *, uint64_t, uint8_t *, size_t, size_t *);
    int32_t (*choose_bias)(void *, uint64_t, uint64_t, uint64_t *);
    int32_t (*map_segment)(void *, const a20_elf_segment_plan_t *, uint64_t);
    int32_t (*finish)(void *, const a20_elf_layout_t *);
    uint8_t expect_class;
    uint8_t wx_xlator_host;
    uint16_t _pad;
    uint16_t expect_machine;
    int32_t wx_policy;
    uint8_t nommu;
    uint8_t _pad1[7];
} a20_elf_interpreter_ops_t;

typedef struct {
    uint64_t key;
    uint64_t val;
} a20_elf_auxv_pair_t;

typedef struct {
    uint64_t phdr_va;
    uint64_t phentsize;
    uint64_t phnum;
    uint64_t page_size;
    uint64_t interp_base;
    uint64_t exec_entry;
    uint64_t hwcap;
    uint64_t hwcap2;
    uint64_t vdso_ehdr;
    uint64_t clktck;
    uint64_t secure;
} a20_elf_auxv_input_t;

typedef struct {
    uint64_t sp;
    uint64_t platform_va;
    uint64_t random_va;
    uint64_t execfn_va;
    uint32_t env_count;
    uint32_t arg_count;
    uint32_t auxv_count;
    uint32_t _pad0;
    uint64_t env_addrs[A20_ELF_MAX_ARG_STRINGS];
    uint64_t arg_addrs[A20_ELF_MAX_ARG_STRINGS];
    a20_elf_auxv_pair_t auxv[A20_ELF_AUXV_MAX_PAIRS];
} a20_elf_stack_plan_t;

typedef struct {
    uint64_t sp;
    uint64_t start_info_va;
    uint64_t argv_va;
    uint64_t envp_va;
    uint32_t argc;
    uint32_t envc;
    uint32_t _pad0;
    uint32_t size;
    uint64_t env_addrs[A20_ELF_MAX_ARG_STRINGS];
    uint64_t arg_addrs[A20_ELF_MAX_ARG_STRINGS];
} a20_elf_start_info_plan_t;

typedef struct {
    uint8_t  no_new_privs;
    uint8_t  _pad0[7];
    int32_t  uid;
    int32_t  euid;
    int32_t  suid;
    int32_t  egid;
    int32_t  fsuid;
    int32_t  fsgid;
    int32_t  sgid;
    int32_t  st_uid;
    int32_t  st_gid;
    uint32_t st_mode;
    uint32_t _pad1;
    uint64_t cap_bounding;
    uint64_t cap_permitted;
    uint64_t cap_effective;
} a20_elf_cred_input_t;

typedef struct {
    int32_t  uid;
    int32_t  euid;
    int32_t  suid;
    int32_t  egid;
    int32_t  fsuid;
    int32_t  fsgid;
    int32_t  sgid;
    uint64_t cap_permitted;
    uint64_t cap_effective;
    uint8_t  make_undumpable;
    uint8_t  _pad0[7];
} a20_elf_cred_plan_t;

typedef struct {
    uint64_t rela;
    uint64_t relasz;
    uint64_t relaent;
    uint64_t symtab;
    uint64_t syment;
    uint64_t hash_sysv;
    uint64_t hash_gnu;
    uint8_t  has_rela;
    uint8_t  _pad0[7];
} a20_elf_dyn_info_t;

typedef struct {
    uint16_t machine;
    uint8_t  abi;
    uint8_t  is_foreign;
    uint8_t  _pad0;
} a20_elf_foreign_key_t;

typedef struct {
    uint8_t  is_script;
    uint8_t  _pad0[7];
    uint32_t interp_off;
    uint32_t interp_len;
    uint32_t arg_off;
    uint32_t arg_len;
} a20_elf_shebang_plan_t;

typedef struct {
    uint32_t new_argc;
    uint8_t  has_arg;
    uint8_t  _pad0[3];
    uint32_t script_pos;
    uint32_t first_orig_src_index;
} a20_elf_shebang_argv_plan_t;

typedef struct {
    uint64_t pages;       /* opaque page-chain handle the adapter allocated */
    uint64_t pages_bytes;
    uint64_t mem;         /* destination physical address */
    uint64_t bufsz;
    uint64_t memsz;
} a20_elf_kexec_slot_t;

/* kexec image/staging ledger. Rust owns every transition (open, accept,
 * attach, publish, release); the C adapter only allocates/frees the chains and
 * does the usercopy. */
typedef struct {
    uint32_t state;       /* A20_ELF_KX_EMPTY / STAGING / LOADED */
    uint32_t file_backed;
    uint32_t nr;          /* segments this load promised */
    uint32_t nstaged;     /* slots recorded so far */
    uint64_t entry;
    a20_elf_kexec_slot_t seg[A20_ELF_KEXEC_MAX_SEGMENTS];
} a20_elf_kexec_ledger_t;

typedef struct {
    uint64_t buf, bufsz, mem, memsz;
} a20_elf_kexec_segment_t;
typedef struct {
    void *context;
    int32_t (*read_segment)(void *, uint32_t, a20_elf_kexec_segment_t *);
    int32_t (*stage_segment)(void *, const a20_elf_kexec_segment_t *, uint64_t *, uint64_t *);
    uint64_t (*lock)(void *);
    void (*unlock)(void *, uint64_t);
    void (*free_pages)(void *, uint64_t);
} a20_elf_kexec_load_ops_t;
typedef struct {
    void *context;
    int32_t (*read_file)(void *, const uint8_t **, uint64_t *);
    int32_t (*stage_file)(void *, const uint8_t *, uint64_t, uint64_t *, uint64_t *);
    void (*free_buffer)(void *, const uint8_t *);
    uint64_t (*lock)(void *);
    void (*unlock)(void *, uint64_t);
    void (*free_pages)(void *, uint64_t);
} a20_elf_kexec_file_ops_t;

#define A20_ELF_KX_EMPTY   0
#define A20_ELF_KX_STAGING 1
#define A20_ELF_KX_LOADED  2

/* ---- C side of the layout contract ---- */
_Static_assert(sizeof(a20_elf_ehdr_t) == 32, "elf_rust ABI drift");
_Static_assert(sizeof(a20_elf_segment_plan_t) == 64, "elf_rust ABI drift");
_Static_assert(sizeof(a20_elf_image_plan_t) ==
                   32 + 8 + A20_ELF_MAX_PHDRS * 64 + 40 + 24 + 24,
               "elf_rust ABI drift");
_Static_assert(sizeof(a20_elf_layout_t) == 72, "elf_rust ABI drift");
_Static_assert(sizeof(a20_elf_load_resources_t) == 144, "elf_rust ABI drift");
_Static_assert(sizeof(a20_elf_load_result_t) == 136, "elf_rust ABI drift");
/* interpreter_ops is the one struct here whose size follows the pointer
 * width: its five function pointers are followed by a u8/u16/i32 tail whose
 * padding depends on where that tail starts.  Every other struct is built
 * from uint64_t members with explicit padding and is width-independent. */
#if __SIZEOF_POINTER__ == 8
_Static_assert(sizeof(a20_elf_interpreter_ops_t) == 64, "elf_rust ABI drift");
#elif __SIZEOF_POINTER__ == 4
_Static_assert(sizeof(a20_elf_interpreter_ops_t) == 40, "elf_rust ABI drift");
#endif
_Static_assert(sizeof(a20_elf_auxv_pair_t) == 16, "elf_rust ABI drift");
_Static_assert(sizeof(a20_elf_auxv_input_t) == 88, "elf_rust ABI drift");
_Static_assert(sizeof(a20_elf_stack_plan_t) ==
                   48 + 2 * A20_ELF_MAX_ARG_STRINGS * 8 +
                       A20_ELF_AUXV_MAX_PAIRS * 16,
               "elf_rust ABI drift");
_Static_assert(sizeof(a20_elf_start_info_plan_t) ==
                   48 + 2 * A20_ELF_MAX_ARG_STRINGS * 8,
               "elf_rust ABI drift");
_Static_assert(sizeof(a20_elf_cred_input_t) == 80, "elf_rust ABI drift");
_Static_assert(sizeof(a20_elf_cred_plan_t) == 56, "elf_rust ABI drift");
_Static_assert(sizeof(a20_elf_dyn_info_t) == 64, "elf_rust ABI drift");
_Static_assert(sizeof(a20_elf_foreign_key_t) == 6, "elf_rust ABI drift");
_Static_assert(sizeof(a20_elf_shebang_plan_t) == 24, "elf_rust ABI drift");
_Static_assert(sizeof(a20_elf_shebang_argv_plan_t) == 16, "elf_rust ABI drift");
_Static_assert(sizeof(a20_elf_kexec_slot_t) == 40, "elf_rust ABI drift");
_Static_assert(sizeof(a20_elf_kexec_ledger_t) ==
                   24 + A20_ELF_KEXEC_MAX_SEGMENTS * 40,
               "elf_rust ABI drift");

/* ---- FFI surface (implemented in kernel/mm/elf_core.rs) ----
 * Convention: every entry returns 0 on success or a negative errno, unless
 * documented otherwise. */

int32_t a20_elf_validate_header(const uint8_t *buf, size_t len,
                                uint8_t expect_class, uint16_t expect_machine);
int32_t a20_elf_plan_image(const uint8_t *ehdr, size_t ehdr_len,
                           const uint8_t *phdr, size_t phdr_len,
                           uint8_t expect_class, uint16_t expect_machine,
                           int32_t wx_policy, bool wx_xlator_host,
                           a20_elf_image_plan_t *out);
typedef int32_t (*a20_elf_map_segment_fn)(void *context,
                                          const a20_elf_segment_plan_t *segment,
                                          uint64_t load_bias);
int32_t a20_elf_map_planned_segments(const a20_elf_image_plan_t *plan,
                                    uint64_t load_bias, void *context,
                                    a20_elf_map_segment_fn map_segment);
int32_t a20_elf_image_span(const a20_elf_image_plan_t *plan,
                           uint64_t *min_out, uint64_t *max_out);
int32_t a20_elf_parse_header(const uint8_t *buf, size_t len, a20_elf_ehdr_t *out);
uint64_t a20_elf_plan_load_bias(uint8_t is_dyn, uint64_t dyn_base,
                                uint64_t aslr_bias);
int32_t a20_elf_finish_layout(const a20_elf_image_plan_t *plan,
                              uint64_t load_bias, a20_elf_layout_t *out);
int32_t a20_elf_load_transaction(const a20_elf_image_plan_t *plan,
                                 const a20_elf_load_transaction_ops_t *ops,
                                 uint64_t *bias_out);
int32_t a20_elf_load_interpreter(const a20_elf_interpreter_ops_t *ops,
                                 uint64_t *entry_out, uint64_t *base_out);
int32_t a20_elf_plan_stack(uint64_t stack_top, size_t ptr_size,
                           const uint8_t *const *argv_ptrs,
                           const size_t *argv_lens, size_t argc,
                           const uint8_t *const *envp_ptrs,
                           const size_t *envp_lens, size_t envc,
                           const uint8_t *platform, size_t platform_len,
                           const uint8_t *execfn, size_t execfn_len,
                           const a20_elf_auxv_input_t *aux,
                           a20_elf_stack_plan_t *out);
int32_t a20_elf_plan_stack_a20(uint64_t stack_top, size_t ptr_size,
                               const uint8_t *const *argv_ptrs,
                               const size_t *argv_lens, size_t argc,
                               const uint8_t *const *envp_ptrs,
                               const size_t *envp_lens, size_t envc,
                               uint64_t start_info_size,
                               a20_elf_start_info_plan_t *out);
typedef int32_t (*a20_elf_stack_copy_fn)(void *context, uint64_t dst_va,
                                         const uint8_t *src, size_t len);
typedef int32_t (*a20_elf_stack_random_fn)(void *context, uint8_t *dst,
                                           size_t len);
int32_t a20_elf_write_linux_stack(uint64_t stack_top, size_t ptr_size,
                                  const uint8_t *const *argv_ptrs,
                                  const size_t *argv_lens, size_t argc,
                                  const uint8_t *const *envp_ptrs,
                                  const size_t *envp_lens, size_t envc,
                                  const uint8_t *platform, size_t platform_len,
                                  const uint8_t *execfn, size_t execfn_len,
                                  const a20_elf_auxv_input_t *aux,
                                  void *context, a20_elf_stack_copy_fn copy,
                                  a20_elf_stack_random_fn random,
                                  uintptr_t *auxv_out, uint32_t *auxv_count_out,
                                  uint64_t *sp_out);
int32_t a20_elf_write_native_stack(uint64_t stack_top, size_t ptr_size,
                                   const uint8_t *const *argv_ptrs,
                                   const size_t *argv_lens, size_t argc,
                                   const uint8_t *const *envp_ptrs,
                                   const size_t *envp_lens, size_t envc,
                                   const uint8_t *start_info,
                                   size_t start_info_size, void *context,
                                   a20_elf_stack_copy_fn copy, uint64_t *sp_out);
uint64_t a20_elf_plan_stack_a20_dynamic(uint64_t stack_top,
                                        uint64_t start_info_size);
int32_t a20_elf_plan_creds(const a20_elf_cred_input_t *in,
                           a20_elf_cred_plan_t *out);
int32_t a20_elf_scan_dynamic(const uint8_t *dyn_bytes, size_t len, bool elf64,
                             a20_elf_dyn_info_t *out);
int32_t a20_elf_probe_foreign(const uint8_t *ehdr, size_t ehdr_len,
                              const uint8_t *phdr, size_t phdr_len,
                              uint8_t expect_class, uint16_t expect_machine,
                              a20_elf_foreign_key_t *out);
int32_t a20_elf_reloc_types(uint16_t machine, bool elf64, uint32_t *out_two);
int32_t a20_elf_reloc_action(uint32_t rel_type, uint32_t symidx, uint64_t nsyms,
                             uint8_t st_info, uint16_t st_shndx,
                             uint32_t want_relative, uint32_t want_absolute);
uint64_t a20_elf_dynsym_count(const uint8_t *sysv, size_t sysv_len,
                              const uint8_t *gnu, size_t gnu_len);
/*
 * Apply the supported relocations of a loaded static PIE to the flat loaded
 * image. Every dynamic/hash/symbol/RELA read and every destination write is
 * bounded against the plan's PT_LOAD mappings by the Rust core; the adapter
 * only hands over the image bytes. Returns the number of stored entries or a
 * negative errno.
 */
int64_t a20_elf_apply_relocs(const a20_elf_image_plan_t *plan, uint64_t load_bias,
                             uint8_t *image, size_t image_len, bool elf64);
int32_t a20_elf_plan_shebang(const uint8_t *line, size_t len,
                             a20_elf_shebang_plan_t *out);
int32_t a20_elf_plan_shebang_argv(bool has_arg, size_t orig_argc,
                                  size_t max_args,
                                  a20_elf_shebang_argv_plan_t *out);
int64_t a20_elf_check_arg_bytes(uint64_t total, uint64_t len,
                                uint64_t max_strlen, uint64_t max_bytes,
                                uint64_t stack_quarter);
int32_t a20_elf_classify(int32_t rc);
int32_t a20_elf_step_after_script(int32_t rc);
int32_t a20_elf_step_after_xlator(bool enabled, int32_t rc);
uint32_t a20_elf_interp_attempts(void);
int32_t a20_elf_interp_resolution(uint32_t index, bool opened);
int32_t a20_elf_kexec_init(a20_elf_kexec_ledger_t *l);
int32_t a20_elf_kexec_stage_open(a20_elf_kexec_ledger_t *l, uint64_t entry,
                                 uint64_t nr_segments, uint64_t max_segments,
                                 bool file_backed);
int32_t a20_elf_kexec_stage_check(a20_elf_kexec_ledger_t *l, uint64_t buf,
                                  uint64_t bufsz, uint64_t mem, uint64_t memsz,
                                  uint64_t kstart, uint64_t kend,
                                  uint32_t *out_index);
int32_t a20_elf_kexec_stage_file(a20_elf_kexec_ledger_t *l, uint64_t size,
                                 uint32_t *out_index);
int32_t a20_elf_kexec_stage_pages(a20_elf_kexec_ledger_t *l, uint32_t index,
                                  uint64_t pages, uint64_t pages_bytes);
int32_t a20_elf_kexec_load_transaction(a20_elf_kexec_ledger_t *dst,
                                       uint64_t entry, uint64_t nr,
                                       uint64_t kstart, uint64_t kend,
                                       const a20_elf_kexec_load_ops_t *ops);
int32_t a20_elf_kexec_discard_transaction(a20_elf_kexec_ledger_t *dst,
                                          const a20_elf_kexec_load_ops_t *ops);
int32_t a20_elf_kexec_file_transaction(a20_elf_kexec_ledger_t *dst,
                                       const a20_elf_kexec_file_ops_t *ops);
int32_t a20_elf_kexec_publish(a20_elf_kexec_ledger_t *dst,
                              a20_elf_kexec_ledger_t *src);
bool a20_elf_kexec_release_next(a20_elf_kexec_ledger_t *l, uint64_t *pages_out,
                                uint64_t *bytes_out);
bool a20_elf_kexec_loaded(const a20_elf_kexec_ledger_t *l);
bool a20_elf_is_elf_image(const uint8_t *buf, size_t len);

/* ---- exec-replacement transaction ---- */

#define A20_ELF_TXN_MAX_RESOURCES 8
#define A20_ELF_TXN_RUNNING       0
#define A20_ELF_TXN_COMMITTED     1
#define A20_ELF_TXN_ABORTED       2
/* `a20_elf_txn_begin` sentinel: every phase has run. */
#define A20_ELF_TXN_DONE          (-2)   /* -ENOENT */

/* Live-resource protocol kinds the executor dispatches on release. */
#define A20_ELF_RES_IMAGE    1
#define A20_ELF_RES_NEW_MM   2
#define A20_ELF_RES_ABI_MODE 3

typedef struct {
    uint32_t kind;
    uint32_t _pad0;
    uint64_t handle;
} a20_elf_txn_resource_t;

typedef struct {
    uint32_t phase;
    uint32_t state;
    uint32_t live;
    /* Rust-owned commit-point boundary: set once the `TrapContext` mm swap has
     * run, after which `a20_elf_txn_abort`/`a20_elf_txn_undo_next` refuse. */
    uint32_t irreversible;
    a20_elf_txn_resource_t res[A20_ELF_TXN_MAX_RESOURCES];
} a20_elf_exec_txn_t;

_Static_assert(sizeof(a20_elf_txn_resource_t) == 16, "elf_rust ABI drift");
_Static_assert(sizeof(a20_elf_exec_txn_t) ==
                   16 + A20_ELF_TXN_MAX_RESOURCES * 16,
               "elf_rust ABI drift");

int32_t a20_elf_txn_init(a20_elf_exec_txn_t *txn);
int32_t a20_elf_txn_begin(a20_elf_exec_txn_t *txn);
int32_t a20_elf_txn_acquire(a20_elf_exec_txn_t *txn, uint32_t kind,
                            uint64_t handle);
int32_t a20_elf_txn_transfer(a20_elf_exec_txn_t *txn, uint32_t kind,
                             uint64_t handle);
int32_t a20_elf_txn_commit(a20_elf_exec_txn_t *txn);
int32_t a20_elf_txn_abort(a20_elf_exec_txn_t *txn);
/* Hand back the newest live resource and drop it from the ledger; false when
 * nothing is left. Drives rollback without re-listing the resource set. */
bool a20_elf_txn_undo_next(a20_elf_exec_txn_t *txn, uint32_t *kind_out,
                           uint64_t *handle_out);
/* Whether a commit phase is failable. 0x100 requests after-effect injection;
 * Rust only permits that for stack, vdso and new-mm preparation. */
bool a20_elf_phase_failable(uint32_t phase);

/* Rust-driven exec sequence. Each callback is one kernel operation over the
 * opaque C task/mm state; Rust owns callback order, resources and rollback. */
typedef int32_t (*a20_elf_exec_action_t)(void *context);
typedef void (*a20_elf_exec_void_action_t)(void *context);
typedef int32_t (*a20_elf_exec_get_abi_t)(void *context);
typedef void (*a20_elf_exec_set_abi_t)(void *context, int32_t mode);
typedef int32_t (*a20_elf_exec_alloc_mm_t)(void *context, uint64_t *handle_out);
typedef void (*a20_elf_exec_undo_t)(void *context, uint32_t kind, uint64_t handle);
typedef bool (*a20_elf_exec_inject_t)(void *context, uint32_t phase, bool after);

typedef struct {
    a20_elf_exec_action_t build_linux_stack;
    a20_elf_exec_action_t build_native_stack;
    a20_elf_exec_action_t map_vdso;
    a20_elf_exec_get_abi_t get_abi_mode;
    a20_elf_exec_set_abi_t set_abi_mode;
    a20_elf_exec_alloc_mm_t allocate_new_mm;
    a20_elf_exec_void_action_t publish_auxv;
    a20_elf_exec_action_t unshare_fdtable;
    a20_elf_exec_void_action_t terminate_siblings;
    a20_elf_exec_void_action_t close_on_exec;
    a20_elf_exec_void_action_t install_trap_context;
    a20_elf_exec_void_action_t apply_credentials;
    a20_elf_exec_void_action_t enter_new_mm;
    a20_elf_exec_undo_t undo_resource;
    a20_elf_exec_inject_t inject_failure;
} a20_elf_exec_ops_t;

int32_t a20_elf_exec_run(uint64_t image, bool native_abi, void *context,
                         const a20_elf_exec_ops_t *ops);

#ifdef __cplusplus
}
#endif

#endif /* _ELF_RUST_H */
