//! Bounded JBD2 v3 record codecs used by ext4 recovery.
//!
//! Rust owns untrusted descriptor/tag/revoke parsing and returns a bounded
//! action list. C keeps block I/O and CRC32C primitives, then applies actions.

const EINVAL: i32 = -22;
const ENOSPC: i32 = -28;
const JBD2_HEADER_BYTES: usize = 12;
const JBD2_TAG3_BYTES: usize = 16;
const JBD2_REVOKE_HEADER_BYTES: usize = 16;
const FLAG_ESCAPE: u32 = 0x1;
const FLAG_SAME_UUID: u32 = 0x2;
const FLAG_DELETED: u32 = 0x4;
const FLAG_LAST_TAG: u32 = 0x8;
const KNOWN_TAG_FLAGS: u32 = FLAG_ESCAPE | FLAG_SAME_UUID | FLAG_DELETED | FLAG_LAST_TAG;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ReplayAction {
    pub target: u64,
    pub checksum: u32,
    pub flags: u32,
}

const _: () = assert!(core::mem::size_of::<ReplayAction>() == 16);

fn be32(bytes: &[u8], offset: usize) -> Option<u32> {
    let end = offset.checked_add(4)?;
    let b = bytes.get(offset..end)?;
    Some(u32::from_be_bytes([b[0], b[1], b[2], b[3]]))
}

fn be64(bytes: &[u8], offset: usize) -> Option<u64> {
    let end = offset.checked_add(8)?;
    let b = bytes.get(offset..end)?;
    Some(u64::from_be_bytes([b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]]))
}

/// Parse one descriptor's usable bytes (header through before checksum tail).
/// The output contains ordered replay actions and is never partially accepted.
pub fn parse_descriptor(bytes: &[u8], has_64bit: bool, filesystem_blocks: u64,
                        output: &mut [ReplayAction]) -> Result<usize, i32> {
    if bytes.len() < JBD2_HEADER_BYTES.checked_add(JBD2_TAG3_BYTES).ok_or(EINVAL)? {
        return Err(EINVAL);
    }
    let mut offset = JBD2_HEADER_BYTES;
    let mut count = 0usize;
    loop {
        let tag_end = offset.checked_add(JBD2_TAG3_BYTES).ok_or(EINVAL)?;
        if tag_end > bytes.len() { return Err(EINVAL); }
        let low = be32(bytes, offset).ok_or(EINVAL)? as u64;
        let flags = be32(bytes, offset.checked_add(4).ok_or(EINVAL)?).ok_or(EINVAL)?;
        let high = be32(bytes, offset.checked_add(8).ok_or(EINVAL)?).ok_or(EINVAL)? as u64;
        let checksum = be32(bytes, offset.checked_add(12).ok_or(EINVAL)?).ok_or(EINVAL)?;
        if flags & !KNOWN_TAG_FLAGS != 0 { return Err(EINVAL); }
        let target = low | if has_64bit { high.checked_shl(32).ok_or(EINVAL)? } else { 0 };
        if target >= filesystem_blocks { return Err(EINVAL); }
        offset = tag_end;
        if flags & FLAG_SAME_UUID == 0 {
            offset = offset.checked_add(16).ok_or(EINVAL)?;
            if offset > bytes.len() { return Err(EINVAL); }
        }
        if count >= output.len() { return Err(ENOSPC); }
        output[count] = ReplayAction { target, checksum, flags };
        count += 1;
        if flags & FLAG_LAST_TAG != 0 { return Ok(count); }
    }
}

/// Decode revoke block targets from the payload ending before its checksum tail.
pub fn parse_revoke(bytes: &[u8], record_bytes: usize, filesystem_blocks: u64,
                    output: &mut [u64]) -> Result<usize, i32> {
    if record_bytes != 4 && record_bytes != 8 { return Err(EINVAL); }
    if bytes.len() < JBD2_REVOKE_HEADER_BYTES { return Err(EINVAL); }
    let count = be32(bytes, 12).ok_or(EINVAL)? as usize;
    if count < JBD2_REVOKE_HEADER_BYTES || count > bytes.len() { return Err(EINVAL); }
    let payload = count.checked_sub(JBD2_REVOKE_HEADER_BYTES).ok_or(EINVAL)?;
    if payload % record_bytes != 0 { return Err(EINVAL); }
    let records = payload / record_bytes;
    if records > output.len() { return Err(ENOSPC); }
    for (index, slot) in output[..records].iter_mut().enumerate() {
        let offset = JBD2_REVOKE_HEADER_BYTES
            .checked_add(index.checked_mul(record_bytes).ok_or(EINVAL)?)
            .ok_or(EINVAL)?;
        let target = if record_bytes == 8 {
            be64(bytes, offset).ok_or(EINVAL)?
        } else {
            be32(bytes, offset).ok_or(EINVAL)? as u64
        };
        if target >= filesystem_blocks { return Err(EINVAL); }
        *slot = target;
    }
    Ok(records)
}

#[no_mangle]
pub unsafe extern "C" fn a20_jbd2_parse_descriptor(bytes: *const u8, len: usize,
        has_64bit: u32, filesystem_blocks: u64, output: *mut ReplayAction,
        capacity: usize, out_count: *mut usize) -> i32 {
    if bytes.is_null() || output.is_null() || out_count.is_null() || has_64bit > 1 {
        return EINVAL;
    }
    let input = unsafe { core::slice::from_raw_parts(bytes, len) };
    let output = unsafe { core::slice::from_raw_parts_mut(output, capacity) };
    match parse_descriptor(input, has_64bit != 0, filesystem_blocks, output) {
        Ok(count) => { unsafe { out_count.write(count) }; 0 }
        Err(error) => error,
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_jbd2_parse_revoke(bytes: *const u8, len: usize,
        record_bytes: usize, filesystem_blocks: u64, output: *mut u64,
        capacity: usize, out_count: *mut usize) -> i32 {
    if bytes.is_null() || output.is_null() || out_count.is_null() { return EINVAL; }
    let input = unsafe { core::slice::from_raw_parts(bytes, len) };
    let output = unsafe { core::slice::from_raw_parts_mut(output, capacity) };
    match parse_revoke(input, record_bytes, filesystem_blocks, output) {
        Ok(count) => { unsafe { out_count.write(count) }; 0 }
        Err(error) => error,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tag(target: u32, flags: u32, checksum: u32) -> [u8; 16] {
        let mut b = [0; 16];
        b[0..4].copy_from_slice(&target.to_be_bytes());
        b[4..8].copy_from_slice(&flags.to_be_bytes());
        b[12..16].copy_from_slice(&checksum.to_be_bytes());
        b
    }

    #[test]
    fn parses_ordered_descriptor_actions_and_optional_uuid() {
        let mut bytes = vec![0u8; 12];
        bytes.extend_from_slice(&tag(7, FLAG_SAME_UUID, 0x1234));
        bytes.extend_from_slice(&tag(9, FLAG_LAST_TAG | FLAG_ESCAPE, 0x5678));
        bytes.extend_from_slice(&[0; 16]);
        let mut actions = [ReplayAction::default(); 4];
        let count = parse_descriptor(&bytes, false, 100, &mut actions).unwrap();
        assert_eq!(count, 2);
        assert_eq!(actions[0], ReplayAction { target: 7, checksum: 0x1234, flags: FLAG_SAME_UUID });
        assert_eq!(actions[1].target, 9);
        assert_eq!(actions[1].flags, FLAG_LAST_TAG | FLAG_ESCAPE);
    }

    #[test]
    fn deleted_tag_remains_an_ordered_data_action() {
        // JBD2 recovery advances one log block for every descriptor tag.
        // DELETED is retained as a tag flag; it does not suppress the data
        // block or shift the following tag's replay action.
        let mut bytes = vec![0u8; 12];
        bytes.extend_from_slice(&tag(7, FLAG_SAME_UUID | FLAG_DELETED, 0x1234));
        bytes.extend_from_slice(&tag(9, FLAG_SAME_UUID | FLAG_LAST_TAG, 0x5678));
        let mut actions = [ReplayAction::default(); 2];
        assert_eq!(parse_descriptor(&bytes, false, 100, &mut actions), Ok(2));
        assert_eq!(actions[0].target, 7);
        assert_eq!(actions[0].flags, FLAG_SAME_UUID | FLAG_DELETED);
        assert_eq!(actions[1].target, 9);
        assert_eq!(actions[1].flags, FLAG_SAME_UUID | FLAG_LAST_TAG);
    }

    #[test]
    fn rejects_truncated_overflow_unknown_flags_and_invalid_targets() {
        let mut bytes = vec![0u8; 12];
        bytes.extend_from_slice(&tag(1, FLAG_SAME_UUID, 0));
        let mut actions = [ReplayAction::default(); 1];
        assert_eq!(parse_descriptor(&bytes[..27], false, 100, &mut actions), Err(EINVAL));
        let mut unknown = vec![0u8; 12];
        unknown.extend_from_slice(&tag(1, FLAG_SAME_UUID | 0x10, 0));
        assert_eq!(parse_descriptor(&unknown, false, 100, &mut actions), Err(EINVAL));
        let mut invalid = vec![0u8; 12];
        invalid.extend_from_slice(&tag(100, FLAG_SAME_UUID | FLAG_LAST_TAG, 0));
        assert_eq!(parse_descriptor(&invalid, false, 100, &mut actions), Err(EINVAL));
        let mut overflow = vec![0u8; 12];
        let mut wide = tag(1, FLAG_SAME_UUID | FLAG_LAST_TAG, 0);
        wide[8..12].copy_from_slice(&1u32.to_be_bytes());
        overflow.extend_from_slice(&wide);
        assert_eq!(parse_descriptor(&overflow, true, u64::MAX, &mut actions), Ok(1));
        assert_eq!(actions[0].target, (1u64 << 32) | 1);
    }

    #[test]
    fn rejects_action_capacity_before_partial_success() {
        let mut bytes = vec![0u8; 12];
        bytes.extend_from_slice(&tag(1, FLAG_SAME_UUID, 0));
        bytes.extend_from_slice(&tag(2, FLAG_SAME_UUID | FLAG_LAST_TAG, 0));
        assert_eq!(parse_descriptor(&bytes, false, 100, &mut [ReplayAction::default(); 1]), Err(ENOSPC));
    }

    #[test]
    fn parses_revoke_width_and_rejects_malformed_counts() {
        let mut bytes = vec![0u8; 24];
        bytes[12..16].copy_from_slice(&24u32.to_be_bytes());
        bytes[16..20].copy_from_slice(&3u32.to_be_bytes());
        bytes[20..24].copy_from_slice(&4u32.to_be_bytes());
        let mut blocks = [0; 2];
        assert_eq!(parse_revoke(&bytes, 4, 100, &mut blocks), Ok(2));
        assert_eq!(blocks, [3, 4]);
        assert_eq!(parse_revoke(&bytes[..23], 4, 100, &mut blocks), Err(EINVAL));
        assert_eq!(parse_revoke(&bytes, 3, 100, &mut blocks), Err(EINVAL));
        let mut overflowing_count = bytes.clone();
        overflowing_count[12..16].copy_from_slice(&u32::MAX.to_be_bytes());
        assert_eq!(parse_revoke(&overflowing_count, 4, 100, &mut blocks), Err(EINVAL));
        let mut out_of_range = bytes.clone();
        out_of_range[16..20].copy_from_slice(&100u32.to_be_bytes());
        assert_eq!(parse_revoke(&out_of_range, 4, 100, &mut blocks), Err(EINVAL));
    }
}
