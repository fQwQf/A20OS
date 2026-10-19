#![no_std]

//! Foreign-architecture translator invocation parsing.
//!
//! Rust owns the two parsers that turn a translator's invocation description
//! into something the exec path can use: the argv template (which substitutions
//! a token carries) and the env spec (which `NAME=VALUE` entries it holds).
//! Both are called twice -- once at boot to validate the configuration, and
//! again on the rare forwarding path -- so they are side-effect free apart from
//! filling the caller's buffer.
//!
//! C keeps everything around them: the guest registry, the configuration
//! table, the boot args walk, the logging and the /proc surface.
//!
//! The behaviour here is a faithful port, including the two quirks worth
//! naming: a literal token longer than 255 bytes is refused because the token
//! window is stored as `uint8_t`, and a token that begins with `@` and carries
//! more than the two substitution characters is split into the first `len - 2`
//! bytes plus the substitution (which is what the glued form resolves to, not
//! a prefix like `--argv0=` that does not itself start with `@`).

#[cfg(test)]
extern crate std;

include!("xlator_tmpl_types.rs");

const EINVAL: i32 = -22;
const E2BIG: i32 = -7;

fn is_sep(c: u8) -> bool {
    c == b' ' || c == b'\t' || c == b','
}

/// Split an argv template into tokens and classify each one.
///
/// Returns the token count, or `-EINVAL` for an unknown `@` token or a
/// template with no `@P` anywhere, or `-E2BIG` past the token or byte bound.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_xlator_parse_template(tmpl: *const u8, out: *mut XlatorTmpl) -> i32 {
    if tmpl.is_null() || out.is_null() {
        return EINVAL;
    }
    let out = unsafe { &mut *out };
    *out = XlatorTmpl {
        text: [0; XLATOR_TMPL_LEN],
        kind: [0; XLATOR_TMPL_MAX],
        start: [0; XLATOR_TMPL_MAX],
        len: [0; XLATOR_TMPL_MAX],
        n: 0,
    };

    let mut len = 0usize;
    while unsafe { *tmpl.add(len) } != 0 {
        len += 1;
    }
    if len >= XLATOR_TMPL_LEN {
        return E2BIG;
    }
    for i in 0..len {
        out.text[i] = unsafe { *tmpl.add(i) };
    }
    out.text[len] = 0;

    let mut has_path = false;
    let mut p = 0usize;
    loop {
        while unsafe { *tmpl.add(p) } != 0 && is_sep(unsafe { *tmpl.add(p) }) {
            p += 1;
        }
        if unsafe { *tmpl.add(p) } == 0 {
            break;
        }
        let start = p;
        while unsafe { *tmpl.add(p) } != 0 && !is_sep(unsafe { *tmpl.add(p) }) {
            p += 1;
        }
        let tlen = p - start;
        if out.n as usize >= XLATOR_TMPL_MAX {
            return E2BIG;
        }

        let first = unsafe { *tmpl.add(start) };
        let second = if tlen >= 2 { unsafe { *tmpl.add(start + 1) } } else { 0 };
        let kind: u8;
        let mut textlen = tlen;

        if tlen == 2 && first == b'@' {
            match second {
                b'A' => kind = XLATOR_TOK_ARGV0,
                b'P' => {
                    kind = XLATOR_TOK_PATH;
                    has_path = true;
                }
                b'*' => kind = XLATOR_TOK_SPLICE,
                _ => return EINVAL,
            }
            textlen = 0;
        } else if tlen >= 2 && first == b'@'
            && (second == b'A' || second == b'P' || second == b'*')
        {
            // A token that begins with a substitution and carries more: the
            // substitution keeps its own argument slot and the leading bytes
            // become a literal token of their own.
            if out.n as usize + 2 > XLATOR_TMPL_MAX {
                return E2BIG;
            }
            let base = out.n as usize;
            out.kind[base] = XLATOR_TOK_LITERAL;
            out.start[base] = start as u8;
            out.len[base] = (tlen - 2) as u8;
            out.n += 1;

            kind = match second {
                b'A' => XLATOR_TOK_ARGV0,
                b'P' => {
                    has_path = true;
                    XLATOR_TOK_PATH
                }
                _ => XLATOR_TOK_SPLICE,
            };
            textlen = 0;
        } else {
            kind = XLATOR_TOK_LITERAL;
            if tlen > 255 {
                return E2BIG;
            }
        }

        let slot = out.n as usize;
        out.kind[slot] = kind;
        if kind == XLATOR_TOK_LITERAL {
            out.start[slot] = start as u8;
            out.len[slot] = textlen as u8;
        } else {
            out.start[slot] = 0;
            out.len[slot] = 0;
        }
        out.n += 1;
    }

    if !has_path {
        return EINVAL;
    }
    out.n
}

/// Split an env spec into `NAME=VALUE` entries, in place.  The entries borrow
/// the caller's scratch buffer, which must be a writable NUL-terminated copy.
///
/// Validation is all-or-nothing: one bad entry rejects the whole spec, because
/// forwarding a half-understood environment is the failure that works on the
/// machine where it was typed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_xlator_split_env(scratch: *mut u8, entries: *mut *mut u8,
                                              max: i32) -> i32 {
    if scratch.is_null() {
        return 0;
    }
    let mut n = 0i32;
    let mut p = 0usize;
    loop {
        while unsafe { *scratch.add(p) } != 0 && is_sep(unsafe { *scratch.add(p) }) {
            unsafe { *scratch.add(p) = 0 };
            p += 1;
        }
        if unsafe { *scratch.add(p) } == 0 {
            break;
        }
        let start = p;
        while unsafe { *scratch.add(p) } != 0 && !is_sep(unsafe { *scratch.add(p) }) {
            p += 1;
        }
        if unsafe { *scratch.add(p) } != 0 {
            unsafe { *scratch.add(p) = 0 };
            p += 1;
        }

        let mut len = 0usize;
        while unsafe { *scratch.add(start + len) } != 0 {
            len += 1;
        }
        if len == 0 {
            continue;
        }
        if len > XLATOR_ENV_ENTRY_MAX {
            return EINVAL;
        }

        let mut eq = start;
        while unsafe { *scratch.add(eq) } != 0 && unsafe { *scratch.add(eq) } != b'=' {
            eq += 1;
        }
        if eq == start || unsafe { *scratch.add(eq) } == 0 {
            return EINVAL; // no '=' at all, or an empty name
        }
        let mut c = start;
        while unsafe { *scratch.add(c) } != 0 {
            let byte = unsafe { *scratch.add(c) };
            if byte < 0x20 || byte == 0x7f {
                return EINVAL;
            }
            c += 1;
        }

        if n >= max {
            return E2BIG;
        }
        if !entries.is_null() {
            unsafe { *entries.add(n as usize) = scratch.add(start) };
        }
        n += 1;
    }
    n
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::format;
    use std::string::String;
    use std::vec::Vec;
    use std::vec;

    fn tmpl_of(text: &str) -> (i32, XlatorTmpl) {
        let mut c = text.as_bytes().to_vec();
        c.push(0);
        let mut out = XlatorTmpl {
            text: [0; XLATOR_TMPL_LEN],
            kind: [0; XLATOR_TMPL_MAX],
            start: [0; XLATOR_TMPL_MAX],
            len: [0; XLATOR_TMPL_MAX],
            n: 0,
        };
        let rc = unsafe { a20_xlator_parse_template(c.as_ptr(), &mut out) };
        (rc, out)
    }

    fn token(t: &XlatorTmpl, i: usize) -> std::string::String {
        let kind = t.kind[i];
        match kind {
            XLATOR_TOK_ARGV0 => "@A".into(),
            XLATOR_TOK_PATH => "@P".into(),
            XLATOR_TOK_SPLICE => "@*".into(),
            _ => {
                let s = t.start[i] as usize;
                let l = t.len[i] as usize;
                std::string::String::from_utf8_lossy(&t.text[s..s + l]).into_owned()
            }
        }
    }

    fn kinds(t: &XlatorTmpl) -> std::vec::Vec<u8> {
        (0..t.n as usize).map(|i| t.kind[i]).collect()
    }

    #[test]
    fn registry_default_parses_into_three_tokens() {
        let (rc, t) = tmpl_of("-0 @A @P @*");
        assert_eq!(rc, 4);
        assert_eq!(kinds(&t), [XLATOR_TOK_LITERAL, XLATOR_TOK_ARGV0,
                               XLATOR_TOK_PATH, XLATOR_TOK_SPLICE]);
        assert_eq!(token(&t, 0), "-0");
        assert_eq!(token(&t, 1), "@A");
        assert_eq!(token(&t, 2), "@P");
        assert_eq!(token(&t, 3), "@*");
    }

    #[test]
    fn separators_are_space_tab_and_comma() {
        let (rc, t) = tmpl_of("@P,@*");
        assert_eq!(rc, 2);
        let (rc, t) = tmpl_of("@P\t@*");
        assert_eq!(rc, 2);
        let (rc, t) = tmpl_of("  @P , @*  ");
        assert_eq!(rc, 2);
        assert_eq!(kinds(&t), [XLATOR_TOK_PATH, XLATOR_TOK_SPLICE]);
    }

    #[test]
    fn a_template_without_path_is_refused() {
        assert_eq!(tmpl_of("@A @*").0, EINVAL);
        assert_eq!(tmpl_of("-0").0, EINVAL);
        assert_eq!(tmpl_of("").0, EINVAL);
    }

    #[test]
    fn an_unknown_substitution_is_refused_rather_than_passed_through() {
        assert_eq!(tmpl_of("@P @X").0, EINVAL);
        // A lone '@' is literal text, not a substitution.
        let (rc, t) = tmpl_of("@P @");
        assert_eq!(rc, 2);
        assert_eq!(token(&t, 1), "@");
        let (rc, t) = tmpl_of("@P a@b");
        assert_eq!(rc, 2);
        assert_eq!(token(&t, 1), "a@b");
    }

    #[test]
    fn a_glued_token_splits_into_literal_plus_substitution() {
        // The ported rule: a token that begins with '@' and carries more than
        // the two substitution characters becomes its first len-2 bytes as a
        // literal plus the substitution named by the second byte.  A token
        // like "@A@P" therefore names no @P at all and is refused.
        assert_eq!(tmpl_of("@A@P").0, EINVAL);
        let (rc, t) = tmpl_of("@Pxyz");
        assert_eq!(rc, 2);
        assert_eq!(kinds(&t), [XLATOR_TOK_LITERAL, XLATOR_TOK_PATH]);
        assert_eq!(token(&t, 0), "@Px");
    }

    #[test]
    fn a_prefix_before_the_substitution_stays_one_literal() {
        // "--argv0=@A" does not begin with '@', so the whole token is literal;
        // this is the ported behaviour, documented in the module header.
        let (rc, t) = tmpl_of("--argv0=@A @P");
        assert_eq!(rc, 2);
        assert_eq!(kinds(&t), [XLATOR_TOK_LITERAL, XLATOR_TOK_PATH]);
        assert_eq!(token(&t, 0), "--argv0=@A");
    }

    #[test]
    fn too_many_tokens_is_e2big() {
        let mut text = std::string::String::new();
        for _ in 0..(XLATOR_TMPL_MAX + 4) {
            text.push_str("@* ");
        }
        text.push_str("@P");
        assert_eq!(tmpl_of(&text).0, E2BIG);
    }

    #[test]
    fn a_template_at_the_byte_bound_is_refused() {
        let long = "x".repeat(XLATOR_TMPL_LEN);
        assert_eq!(tmpl_of(&long).0, E2BIG);
        let ok = format!("{} @P", "x".repeat(XLATOR_TMPL_LEN - 6));
        assert_eq!(tmpl_of(&ok).0, 2);
    }

    #[test]
    fn a_template_longer_than_the_buffer_is_refused() {
        // A token cannot exceed 255 bytes without the template exceeding the
        // 256-byte buffer first, so the per-token bound is unreachable and the
        // template bound is what a caller can actually hit.
        let text = format!("{} @P", "y".repeat(300));
        assert_eq!(tmpl_of(&text).0, E2BIG);
    }

    #[test]
    fn a_null_template_is_refused() {
        let mut out = XlatorTmpl {
            text: [0; XLATOR_TMPL_LEN],
            kind: [0; XLATOR_TMPL_MAX],
            start: [0; XLATOR_TMPL_MAX],
            len: [0; XLATOR_TMPL_MAX],
            n: 0,
        };
        assert_eq!(unsafe { a20_xlator_parse_template(core::ptr::null(), &mut out) }, EINVAL);
    }

    fn split(spec: &str, max: i32) -> (i32, std::vec::Vec<std::string::String>) {
        let mut scratch = spec.as_bytes().to_vec();
        scratch.push(0);
        let mut entries = [core::ptr::null_mut::<u8>(); XLATOR_ENV_MAX + 2];
        let rc = unsafe {
            a20_xlator_split_env(scratch.as_mut_ptr(), entries.as_mut_ptr(), max)
        };
        let mut out = std::vec::Vec::new();
        if rc > 0 {
            for i in 0..rc as usize {
                let mut len = 0usize;
                while unsafe { *entries[i].add(len) } != 0 {
                    len += 1;
                }
                let bytes = unsafe { core::slice::from_raw_parts(entries[i], len) };
                out.push(std::string::String::from_utf8_lossy(bytes).into_owned());
            }
        }
        (rc, out)
    }

    #[test]
    fn env_spec_splits_on_commas_and_spaces() {
        let (rc, entries) = split("A=1,B=2 C=3", XLATOR_ENV_MAX as i32);
        assert_eq!(rc, 3);
        assert_eq!(entries, ["A=1", "B=2", "C=3"]);
    }

    #[test]
    fn an_empty_spec_yields_no_entries() {
        assert_eq!(split("", XLATOR_ENV_MAX as i32).0, 0);
        assert_eq!(split(" , , ", XLATOR_ENV_MAX as i32).0, 0);
    }

    #[test]
    fn an_entry_without_a_name_is_refused() {
        assert_eq!(split("=1", XLATOR_ENV_MAX as i32).0, EINVAL);
        assert_eq!(split("A=1,=2", XLATOR_ENV_MAX as i32).0, EINVAL);
        assert_eq!(split("NOVALUE", XLATOR_ENV_MAX as i32).0, EINVAL);
    }

    #[test]
    fn a_control_character_is_refused() {
        assert_eq!(split("A=1,B=\x01", XLATOR_ENV_MAX as i32).0, EINVAL);
        assert_eq!(split("A=1,B=\x7f", XLATOR_ENV_MAX as i32).0, EINVAL);
    }

    #[test]
    fn an_over_long_entry_is_refused() {
        let long = format!("A={}", "v".repeat(XLATOR_ENV_ENTRY_MAX));
        assert_eq!(split(&long, XLATOR_ENV_MAX as i32).0, EINVAL);
        let ok = format!("A={}", "v".repeat(XLATOR_ENV_ENTRY_MAX - 2));
        assert_eq!(split(&ok, XLATOR_ENV_MAX as i32).0, 1);
    }

    #[test]
    fn too_many_entries_is_e2big() {
        let spec = "A=1,B=2,C=3";
        assert_eq!(split(spec, 2).0, E2BIG);
        assert_eq!(split(spec, 3).0, 3);
    }

    #[test]
    fn a_value_may_contain_equals_signs() {
        let (rc, entries) = split("A=1=2", XLATOR_ENV_MAX as i32);
        assert_eq!(rc, 1);
        assert_eq!(entries, ["A=1=2"]);
    }
}
