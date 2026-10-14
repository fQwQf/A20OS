//! Explicit read-only downgrade gate for legacy JBD2 checksum formats.
//!
//! The user filesystem service may mount a clean v1/v2 journal read-only. It
//! never replays a legacy journal: a non-empty log remains an explicit mount
//! error until compatible checksum verification/replay is implemented.

const COMPAT_CHECKSUM_V1: u32 = 0x1;
const INCOMPAT_REVOKE: u32 = 0x1;
const INCOMPAT_64BIT: u32 = 0x2;
const INCOMPAT_ASYNC_COMMIT: u32 = 0x4;
const INCOMPAT_CHECKSUM_V2: u32 = 0x8;
const INCOMPAT_CHECKSUM_V3: u32 = 0x10;
const INCOMPAT_FAST_COMMIT: u32 = 0x20;
const KNOWN_BASE_INCOMPAT: u32 = INCOMPAT_REVOKE | INCOMPAT_64BIT;
const CRC32C: u32 = 4;

/// Return 1 only for a structurally valid, empty v1/v2 journal requested as
/// read-only. Return 0 for all other feature combinations and dirty logs.
fn allow_clean_readonly(
    read_only: bool,
    compat: u32,
    incompat: u32,
    ro_compat: u32,
    checksum_type: u32,
    block_size: u32,
    maxlen: u32,
    first: u32,
    start: u32,
    fs_block_size: u32,
    inode_blocks: u64,
) -> bool {
    if !read_only || ro_compat != 0 || block_size != fs_block_size ||
       block_size != 4096 || maxlen < 2 || maxlen > 65536 ||
       maxlen as u64 > inode_blocks || first < 1 || first >= maxlen ||
       start != 0 || (incompat & (INCOMPAT_ASYNC_COMMIT | INCOMPAT_FAST_COMMIT)) != 0 ||
       (incompat & !(KNOWN_BASE_INCOMPAT | INCOMPAT_CHECKSUM_V2 |
                     INCOMPAT_CHECKSUM_V3)) != 0 {
        return false;
    }

    let v1 = compat == COMPAT_CHECKSUM_V1 &&
             (incompat & (INCOMPAT_CHECKSUM_V2 | INCOMPAT_CHECKSUM_V3)) == 0;
    let v2 = compat == 0 && (incompat & INCOMPAT_CHECKSUM_V2) != 0 &&
             (incompat & INCOMPAT_CHECKSUM_V3) == 0 && checksum_type == CRC32C;
    v1 || v2
}

#[no_mangle]
pub extern "C" fn a20_jbd2_allow_clean_legacy_readonly(
    read_only: i32, compat: u32, incompat: u32, ro_compat: u32,
    checksum_type: u32, block_size: u32, maxlen: u32, first: u32,
    start: u32, fs_block_size: u32, inode_blocks: u64,
) -> i32 {
    i32::from(allow_clean_readonly(
        read_only != 0, compat, incompat, ro_compat, checksum_type,
        block_size, maxlen, first, start, fs_block_size, inode_blocks,
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn base(read_only: bool, compat: u32, incompat: u32, start: u32) -> bool {
        allow_clean_readonly(read_only, compat, incompat, 0, 4, 4096,
                             1024, 1, start, 4096, 4096)
    }

    #[test]
    fn clean_v1_is_read_only_only() {
        assert!(base(true, COMPAT_CHECKSUM_V1, INCOMPAT_REVOKE, 0));
        assert!(!base(false, COMPAT_CHECKSUM_V1, INCOMPAT_REVOKE, 0));
    }

    #[test]
    fn clean_v2_requires_crc32c_and_no_v3() {
        assert!(base(true, 0, INCOMPAT_REVOKE | INCOMPAT_CHECKSUM_V2, 0));
        assert!(!base(true, 0, INCOMPAT_REVOKE | INCOMPAT_CHECKSUM_V2 |
                       INCOMPAT_CHECKSUM_V3, 0));
        assert!(!allow_clean_readonly(true, 0, INCOMPAT_CHECKSUM_V2, 0, 1,
                         4096, 1024, 1, 0, 4096, 4096));
    }

    #[test]
    fn dirty_or_unsupported_journals_never_downgrade() {
        assert!(!base(true, COMPAT_CHECKSUM_V1, INCOMPAT_REVOKE, 2));
        assert!(!base(true, COMPAT_CHECKSUM_V1, INCOMPAT_REVOKE |
                       INCOMPAT_ASYNC_COMMIT, 0));
        assert!(!base(true, COMPAT_CHECKSUM_V1, INCOMPAT_REVOKE |
                       INCOMPAT_FAST_COMMIT, 0));
        assert!(!base(true, COMPAT_CHECKSUM_V1, INCOMPAT_REVOKE | 0x80, 0));
    }

    #[test]
    fn malformed_geometry_and_checksum_features_reject() {
        assert!(!allow_clean_readonly(true, COMPAT_CHECKSUM_V1, INCOMPAT_REVOKE,
                         0, 4, 1024, 1024, 1, 0, 4096, 4096));
        assert!(!allow_clean_readonly(true, COMPAT_CHECKSUM_V1, INCOMPAT_REVOKE,
                         0, 4, 4096, 1024, 1, 0, 4096, 8));
        assert!(!allow_clean_readonly(true, COMPAT_CHECKSUM_V1, INCOMPAT_REVOKE,
                         1, 4, 4096, 1024, 1, 0, 4096, 4096));
    }
}
