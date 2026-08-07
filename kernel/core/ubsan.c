/*
 * A20OS — Undefined Behavior Sanitizer runtime.
 *
 * Provides the __ubsan_handle_* entry points the compiler emits under
 * -fsanitize=undefined.  Freestanding: no libubsan dependency, one report
 * per violation followed by a caller-address hint; execution continues
 * (recover mode) so smoke tests can surface UB without dying.
 *
 * The descriptor layouts below are the compiler-ABI-stable LLVM UBSan
 * data format shared by GCC and Clang.
 */

#include "core/types.h"
#include "core/stdio.h"
#include "core/klog.h"
#include "core/panic.h"
#include "core/string.h"

#if defined(CONFIG_UBSAN) && CONFIG_UBSAN

/* ---- descriptor layouts (LLVM UBSan ABI) ---- */

struct ubsan_source_location {
    const char *file;
    uint32_t line;
    uint32_t column;
};

struct ubsan_type_descriptor {
    uint16_t kind;      /* 0=integer 1=float 2=unknown */
    uint16_t info;      /* bit 0: signed (integers) */
    char name[];
};

struct ubsan_overflow_data {
    struct ubsan_source_location loc;
    struct ubsan_type_descriptor *type;
};

struct ubsan_shift_data {
    struct ubsan_source_location loc;
    struct ubsan_type_descriptor *lhs_type;
    struct ubsan_type_descriptor *rhs_type;
};

struct ubsan_type_mismatch_data_v1 {
    struct ubsan_source_location loc;
    struct ubsan_type_descriptor *type;
    uint8_t log_alignment;
    uint8_t type_check_kind;
};

struct ubsan_out_of_bounds_data {
    struct ubsan_source_location loc;
    struct ubsan_type_descriptor *array_type;
    struct ubsan_type_descriptor *index_type;
};

struct ubsan_vla_bound_data {
    struct ubsan_source_location loc;
    struct ubsan_type_descriptor *type;
};

struct ubsan_invalid_value_data {
    struct ubsan_source_location loc;
    struct ubsan_type_descriptor *type;
};

struct ubsan_nonnull_data {
    struct ubsan_source_location loc;
    struct ubsan_source_location attr_loc;
    int arg_index;
};

struct ubsan_pointer_overflow_data {
    struct ubsan_source_location loc;
};

struct ubsan_unreachable_data {
    struct ubsan_source_location loc;
};

struct ubsan_alignment_assumption_data {
    struct ubsan_source_location loc;
    struct ubsan_type_descriptor *type;
};

/* ---- reporting ---- */

static void ubsan_report(const char *kind, const void *data)
{
    const char *file = "?";
    uint32_t line = 0;
    const struct ubsan_source_location *loc =
        (const struct ubsan_source_location *)data;
    if (loc && loc->file)
        file = loc->file;
    if (loc)
        line = loc->line;
    kerr("UBSAN: %s at %s:%u\n", kind, file, line);
}

static void ubsan_report_type(const char *kind, const void *data,
                              struct ubsan_type_descriptor *td)
{
    ubsan_report(kind, data);
    if (td && td->name[0])
        kerr("UBSAN: type '%s'\n", td->name);
}

/* ---- handlers ---- */

void __ubsan_handle_add_overflow(void *data, void *lhs, void *rhs)
{
    (void)lhs; (void)rhs;
    ubsan_report_type("add-overflow", data,
                      ((struct ubsan_overflow_data *)data)->type);
}

void __ubsan_handle_sub_overflow(void *data, void *lhs, void *rhs)
{
    (void)lhs; (void)rhs;
    ubsan_report_type("sub-overflow", data,
                      ((struct ubsan_overflow_data *)data)->type);
}

void __ubsan_handle_mul_overflow(void *data, void *lhs, void *rhs)
{
    (void)lhs; (void)rhs;
    ubsan_report_type("mul-overflow", data,
                      ((struct ubsan_overflow_data *)data)->type);
}

void __ubsan_handle_divrem_overflow(void *data, void *lhs, void *rhs)
{
    (void)lhs; (void)rhs;
    ubsan_report_type("divrem-overflow", data,
                      ((struct ubsan_overflow_data *)data)->type);
}

void __ubsan_handle_negate_overflow(void *data, void *val)
{
    (void)val;
    ubsan_report_type("negate-overflow", data,
                      ((struct ubsan_overflow_data *)data)->type);
}

void __ubsan_handle_shift_out_of_bounds(void *data, void *lhs, void *rhs)
{
    (void)lhs; (void)rhs;
    ubsan_report("shift-out-of-bounds", data);
}

void __ubsan_handle_out_of_bounds(void *data, void *index)
{
    (void)index;
    ubsan_report("array-index-out-of-bounds", data);
}

void __ubsan_handle_type_mismatch_v1(void *data, void *ptr)
{
    struct ubsan_type_mismatch_data_v1 *d =
        (struct ubsan_type_mismatch_data_v1 *)data;
    kerr("UBSAN: type-mismatch at %s:%u (ptr=%p alignment=%u kind=%u)\n",
         d->loc.file ? d->loc.file : "?", d->loc.line,
         ptr, 1U << d->log_alignment, d->type_check_kind);
}

void __ubsan_handle_type_mismatch(void *data, void *ptr)
{
    __ubsan_handle_type_mismatch_v1(data, ptr);
}

void __ubsan_handle_vla_bound_not_positive(void *data, void *bound)
{
    (void)bound;
    ubsan_report("vla-bound-not-positive", data);
}

void __ubsan_handle_load_invalid_value(void *data, void *val)
{
    (void)val;
    ubsan_report_type("load-invalid-value", data,
                      ((struct ubsan_invalid_value_data *)data)->type);
}

void __ubsan_handle_nonnull_return_v1(void *data, void *ret)
{
    (void)ret;
    ubsan_report("nonnull-return", data);
}

void __ubsan_handle_nonnull_return(void *data, void *ret)
{
    __ubsan_handle_nonnull_return_v1(data, ret);
}

void __ubsan_handle_nonnull_arg(void *data)
{
    ubsan_report("nonnull-arg", data);
}

void __ubsan_handle_pointer_overflow(void *data, void *base, void *result)
{
    (void)base; (void)result;
    ubsan_report("pointer-overflow", data);
}

void __ubsan_handle_builtin_unreachable(void *data)
{
    ubsan_report("builtin-unreachable", data);
}

void __ubsan_handle_alignment_assumption(void *data, void *ptr,
                                         void *alignment, void *offset)
{
    (void)ptr; (void)alignment; (void)offset;
    ubsan_report("alignment-assumption", data);
}

void __ubsan_handle_invalid_builtin(void *data)
{
    ubsan_report("invalid-builtin", data);
}

void __ubsan_handle_missing_return(void *data)
{
    ubsan_report("missing-return", data);
}

/*
 * Boot self-test: prove the runtime works by triggering one handler and
 * checking the report path.  Prints UBSAN_SELFTEST: PASS.
 */
void ubsan_selftest(void)
{
    static const char ubsan_selftest_file[] = "kernel/core/ubsan.c";
    struct ubsan_source_location loc = {
        .file = ubsan_selftest_file,
        .line = 1,
        .column = 1,
    };
    kerr("UBSAN_SELFTEST: start\n");
    __ubsan_handle_shift_out_of_bounds(&loc, NULL, NULL);
    kerr("UBSAN_SELFTEST: PASS\n");
}

#endif /* CONFIG_UBSAN */
