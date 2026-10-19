// Shared between kernel/proc/xlator_tmpl.rs (the parsers) and
// kernel/proc/xlator_config.rs (the configuration pass that calls them), so
// the template object's layout and the bounds cannot diverge.

pub const XLATOR_TMPL_LEN: usize = 256;
pub const XLATOR_TMPL_MAX: usize = 32;
pub const XLATOR_ENV_MAX: usize = 8;
pub const XLATOR_ENV_ENTRY_MAX: usize = 64;
pub const XLATOR_ENV_LEN: usize = XLATOR_ENV_MAX * (XLATOR_ENV_ENTRY_MAX + 1);

pub const XLATOR_TOK_LITERAL: u8 = 0;
pub const XLATOR_TOK_ARGV0: u8 = 1;
pub const XLATOR_TOK_PATH: u8 = 2;
pub const XLATOR_TOK_SPLICE: u8 = 3;

/// The C `xlator_tmpl_t`: one fixed-size object the exec path keeps on its
/// stack, with each token's text living in `text` at its (start, len) window.
#[repr(C)]
pub struct XlatorTmpl {
    pub text: [u8; XLATOR_TMPL_LEN],
    pub kind: [u8; XLATOR_TMPL_MAX],
    pub start: [u8; XLATOR_TMPL_MAX],
    pub len: [u8; XLATOR_TMPL_MAX],
    pub n: i32,
}

const _: () = {
    assert!(core::mem::size_of::<XlatorTmpl>() == 356);
};
