//! Wire layout and checked frame arithmetic shared by the Rust ufsd loop.

pub const REQ_MAGIC: u32 = 0x5546_5352;
pub const RESP_MAGIC: u32 = 0x5546_5350;
pub const PROTO_VERSION: u32 = 1;
pub const ROOT_INO: u64 = 1;
pub const OP_INIT: u32 = 1;
pub const OP_LOOKUP: u32 = 2;
pub const OP_GETATTR: u32 = 3;
pub const OP_READDIR: u32 = 4;
pub const OP_CREATE: u32 = 5;
pub const OP_MKDIR: u32 = 6;
pub const OP_UNLINK: u32 = 7;
pub const OP_RMDIR: u32 = 8;
pub const OP_TRUNCATE: u32 = 9;
pub const OP_READ: u32 = 10;
pub const OP_WRITE: u32 = 11;
pub const OP_SYNC: u32 = 12;
pub const OP_STATFS: u32 = 13;
pub const OP_RENAME: u32 = 14;
pub const MAX_MESSAGE: usize = 65536;
pub const MAX_NAME: usize = 511;

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Request {
    pub magic: u32,
    pub version: u32,
    pub opcode: u32,
    pub req_id: u32,
    pub ino: u64,
    pub arg0: u64,
    pub arg1: u64,
    pub name_len: u32,
    pub payload_len: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Response {
    pub magic: u32,
    pub opcode: u32,
    pub req_id: u32,
    pub status: i32,
    pub out0: u64,
    pub out1: u64,
    pub out2: u64,
    pub payload_len: u32,
    pub pad: u32,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct FrameLayout {
    pub name: core::ops::Range<usize>,
    pub payload: core::ops::Range<usize>,
}

pub fn validate_frame(frame_len: usize, request: &Request) -> Option<FrameLayout> {
    let header_len = core::mem::size_of::<Request>();
    let name_start = header_len;
    let payload_start = name_start.checked_add(request.name_len as usize)?;
    let frame_end = payload_start.checked_add(request.payload_len as usize)?;
    if frame_end != frame_len || frame_end > MAX_MESSAGE {
        return None;
    }
    if request.name_len as usize > MAX_NAME {
        return None;
    }
    Some(FrameLayout {
        name: name_start..payload_start,
        payload: payload_start..frame_end,
    })
}

pub fn valid_name(bytes: &[u8]) -> bool {
    if bytes.len() > MAX_NAME { return false; }
    for byte in bytes { if *byte == 0 { return false; } }
    true
}

pub fn parse_block_index(bytes: &[u8]) -> Option<i32> {
    if bytes.is_empty() {
        return None;
    }
    let mut value = 0i32;
    for byte in bytes {
        if !byte.is_ascii_digit() {
            return None;
        }
        value = value.checked_mul(10)?.checked_add((byte - b'0') as i32)?;
    }
    Some(value)
}

pub fn backend_for(name: &[u8]) -> Option<u32> {
    match name {
        b"" | b"fat" => Some(1),
        b"ext4" | b"ntfs" | b"iso9660" => Some(2),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn frame_bounds_are_checked_before_name_or_payload_slices() {
        let req = Request { name_len: 4, payload_len: 8, ..Request::default() };
        let header = core::mem::size_of::<Request>();
        assert_eq!(validate_frame(header + 12, &req).unwrap().payload, header + 4..header + 12);
        assert!(validate_frame(header + 11, &req).is_none());
        assert!(validate_frame(header, &Request { name_len: u32::MAX, ..req }).is_none());
        assert!(validate_frame(MAX_MESSAGE + 1, &Request { name_len: (MAX_NAME + 1) as u32, ..req }).is_none());
        assert!(validate_frame(header + 13, &req).is_none());
    }

    #[test]
    fn backend_names_are_bounded_and_c_string_safe() {
        assert!(valid_name(b"file.txt"));
        assert!(!valid_name(b"a\0b"));
        assert!(!valid_name(&[b'x'; MAX_NAME + 1]));
    }

    #[test]
    fn block_index_requires_complete_checked_decimal_token() {
        assert_eq!(parse_block_index(b"15"), Some(15));
        for invalid in [&b""[..], b"-1", b"1junk", b"2147483648", b"999999999999999999999"] {
            assert_eq!(parse_block_index(invalid), None);
        }
    }

    #[test]
    fn filesystem_personality_is_selected_by_rust_policy() {
        assert_eq!(backend_for(b""), Some(1));
        assert_eq!(backend_for(b"fat"), Some(1));
        for fs in [b"ext4" as &[u8], b"ntfs", b"iso9660"] {
            assert_eq!(backend_for(fs), Some(2));
        }
        assert_eq!(backend_for(b"unknown"), None);
    }
}
