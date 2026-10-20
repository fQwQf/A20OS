// Host oracle driver for the ELF parser fixture set.
//
// Usage:  oracle <fixtures-dir> <rust-expectations.tsv>
//
// Replays every non-skip row of rust-expectations.tsv against a snapshot of
// kernel/mm/elf_core.rs (compiled as elf_core_snapshot). A row that models an
// adapter-level gate (parse=0) is checked without calling plan_image, exactly
// the way the C adapter reads the file before planning. skip=1 rows belong to
// the path/bridge context and are counted but not executed.
//
// This host-side model replays vectors against the crate snapshot; it is not
// a substitute for testing the live kernel loader.

use std::fs;
use std::process::exit;

use elf_core_snapshot::{
    plan_image, EHDR32_SIZE, EHDR64_SIZE, MM_WX_DENY, MM_WX_OFF, MM_WX_STRIP, PROT_EXEC,
    PROT_READ, PROT_WRITE,
};

struct Row {
    id: String,
    parse: i32,
    code_off: i32,
    code_strip: i32,
    code_deny: i32,
    nseg: u32,
    is_native: u8,
    skip: u8,
    target_class: u8,
    target_machine: u16,
    strip_prot: u8,
}

fn parse_row(line: &str) -> Option<Row> {
    let f: Vec<&str> = line.split('\t').collect();
    if f.len() < 11 {
        return None;
    }
    Some(Row {
        id: f[0].to_string(),
        parse: f[1].parse().ok()?,
        code_off: f[2].parse().ok()?,
        code_strip: f[3].parse().ok()?,
        code_deny: f[4].parse().ok()?,
        nseg: f[5].parse().ok()?,
        is_native: f[6].parse().ok()?,
        skip: f[7].parse().ok()?,
        target_class: f[8].parse().ok()?,
        target_machine: f[9].parse().ok()?,
        strip_prot: f[10].parse().ok()?,
    })
}

/// Adapter-side gate: pull the phdr table slice out of a whole-file buffer the
/// way the C loader does (e_phoff is *not* re-applied by plan_image; the
/// adapter hands it the bytes starting at the table). The slice is everything
/// physically present from e_phoff on — plan_image re-checks the table length
/// itself. Returns None only when the file is too small for a full ehdr of
/// its class — the adapter must refuse with -ENOEXEC before planning.
fn table_slice(buf: &[u8]) -> Option<&[u8]> {
    if buf.len() < 6 {
        return None;
    }
    let class = buf[4];
    let (poff, ehdr_need) = match class {
        2 => (
            u64::from_le_bytes(to8(&buf[32..40])) as usize,
            EHDR64_SIZE as usize,
        ),
        1 => (
            u32::from_le_bytes(to4(&buf[28..32])) as usize,
            EHDR32_SIZE as usize,
        ),
        _ => return None,
    };
    if buf.len() < ehdr_need {
        return None;
    }
    Some(buf.get(poff..).unwrap_or(&[]))
}

fn to8(b: &[u8]) -> [u8; 8] {
    [b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]]
}
fn to4(b: &[u8]) -> [u8; 4] {
    [b[0], b[1], b[2], b[3]]
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 3 {
        eprintln!("usage: {} <fixtures-dir> <rust-expectations.tsv>", args[0]);
        exit(2);
    }
    let (dir, tsv) = (&args[1], &args[2]);
    let text = match fs::read_to_string(tsv) {
        Ok(t) => t,
        Err(e) => {
            eprintln!("cannot read {}: {}", tsv, e);
            exit(2);
        }
    };

    let (mut pass, mut fail, mut skip) = (0u32, 0u32, 0u32);
    for line in text.lines().skip(1) {
        if line.is_empty() {
            continue;
        }
        let row = match parse_row(line) {
            Some(r) => r,
            None => {
                println!("SKIP  (unparseable row: {:?})", line);
                skip += 1;
                continue;
            }
        };
        if row.skip == 1 {
            println!("SKIP  {} (path/bridge context, not plan API)", row.id);
            skip += 1;
            continue;
        }
        let path = format!("{}/{}.elf", dir, row.id);
        let buf = match fs::read(&path) {
            Ok(b) => b,
            Err(e) => {
                println!("FAIL  {}: fixture unreadable: {}", row.id, e);
                fail += 1;
                continue;
            }
        };

        if row.parse == 0 {
            // Adapter-level gate: no plan_image call. The model only promises
            // -ENOEXEC for header-buffer damage; verify the file shape agrees
            // (file shorter than its class' full ehdr).
            let gate_ok = table_slice(&buf).is_none() && row.code_off == -8;
            if gate_ok {
                println!("PASS  {} (adapter gate, no plan_image call)", row.id);
                pass += 1;
            } else {
                println!("FAIL  {}: adapter gate mismatch", row.id);
                fail += 1;
            }
            continue;
        }

        let phdr = match table_slice(&buf) {
            Some(p) => p,
            None => {
                println!("FAIL  {}: parse=1 but adapter gate would reject", row.id);
                fail += 1;
                continue;
            }
        };
        let ehdr_all: &[u8] = &buf;

        // Three wx-policy calls, one expectation column each; the row counts
        // once, on its strictest verdict.
        let mut row_ok = true;
        let mut detail = String::new();
        for (policy, label, want) in [
            (MM_WX_OFF, "off", row.code_off),
            (MM_WX_STRIP, "strip", row.code_strip),
            (MM_WX_DENY, "deny", row.code_deny),
        ] {
            match plan_image(
                ehdr_all,
                phdr,
                row.target_class,
                row.target_machine,
                policy,
                false,
            ) {
                Ok(plan) => {
                    if want != 0 {
                        detail = format!("wx={} expected {} got Ok(nseg={})",
                                         label, want, plan.nseg);
                        row_ok = false;
                        break;
                    }
                    if plan.nseg != row.nseg || plan.is_native_abi != row.is_native {
                        detail = format!(
                            "wx={}: nseg/is_native {}={}, want {}/{}",
                            label, plan.nseg, plan.is_native_abi,
                            row.nseg, row.is_native
                        );
                        row_ok = false;
                        break;
                    }
                    if row.strip_prot == 1 && policy == MM_WX_STRIP {
                        // Stripped plan must hold no W|X anywhere and the
                        // first segment must be plain R|X.
                        let rwx_gone = plan.segments[..plan.nseg as usize]
                            .iter()
                            .all(|s| s.prot & (PROT_WRITE | PROT_EXEC)
                                != (PROT_WRITE | PROT_EXEC));
                        let first_rx = plan.segments[0].prot == (PROT_READ | PROT_EXEC);
                        if !(rwx_gone && first_rx) {
                            detail = format!(
                                "wx=strip prot check failed (prot0={})",
                                plan.segments[0].prot
                            );
                            row_ok = false;
                            break;
                        }
                    }
                }
                Err(code) => {
                    // The snapshot's Err payloads are inconsistent by
                    // convention: validation paths carry the positive errno
                    // (Err(ENOEXEC)), while wx deny carries the signed value
                    // (-EACCES). Canonicalize to the errno-sign convention
                    // the expectation TSV uses.
                    let canon = if code > 0 { -code } else { code };
                    if canon != want {
                        detail = format!("wx={} expected {} got {} (raw {})",
                                         label, want, canon, code);
                        row_ok = false;
                        break;
                    }
                }
            }
        }
        if row_ok {
            println!("PASS  {}", row.id);
            pass += 1;
        } else {
            println!("FAIL  {}: {}", row.id, detail);
            fail += 1;
        }
    }

    println!("pass={} fail={} skip={}", pass, fail, skip);
    if fail != 0 {
        exit(1);
    }
}
