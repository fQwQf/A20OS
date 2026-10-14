//! Pure validation for the v1-envelope service-version negotiation payload.
//! Kept independent of syscall and channel code so malformed ACKs can be
//! checked on the host as well as by the freestanding client.

pub const STATUS_OK: u16 = 0;
pub const STATUS_NO_COMMON: u16 = 1;
pub const STATUS_INVALID_RANGE: u16 = 2;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum NegotiationError {
    InvalidClientRange,
    InvalidServerRange,
    NonceMismatch,
    InvalidStatus,
    InconsistentStatus,
    WrongSelection,
}

/// The compatibility baseline is the existing v1 echo service. A full echo
/// is insufficient to downgrade a client whose advertised range excludes v1.
pub fn legacy_v1_in_range(client_min: u16, client_max: u16) -> bool {
    client_min != 0 && client_min <= client_max &&
        client_min <= 1 && 1 <= client_max
}

/// Validate the response against the request. A successful peer must select
/// the highest version in the intersection; an error must select zero.
/// Server `min_version`/`max_version` describe its supported range.
pub fn validate_response(
    client_min: u16,
    client_max: u16,
    nonce: u32,
    server_min: u16,
    server_max: u16,
    selected: u16,
    status: u16,
    response_nonce: u32,
) -> Result<Option<u16>, NegotiationError> {
    if client_min == 0 || client_min > client_max {
        return Err(NegotiationError::InvalidClientRange);
    }
    if server_min == 0 || server_min > server_max {
        return Err(NegotiationError::InvalidServerRange);
    }
    if nonce != response_nonce {
        return Err(NegotiationError::NonceMismatch);
    }
    let common_min = core::cmp::max(client_min, server_min);
    let common_max = core::cmp::min(client_max, server_max);
    match status {
        STATUS_OK => {
            if common_min > common_max {
                return Err(NegotiationError::InconsistentStatus);
            }
            if selected != common_max {
                return Err(NegotiationError::WrongSelection);
            }
            Ok(Some(selected))
        }
        STATUS_NO_COMMON => {
            if common_min <= common_max || selected != 0 {
                return Err(NegotiationError::InconsistentStatus);
            }
            Ok(None)
        }
        STATUS_INVALID_RANGE => Err(NegotiationError::InvalidStatus),
        _ => Err(NegotiationError::InvalidStatus),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const CLIENT_MIN: u16 = 1;
    const CLIENT_MAX: u16 = 3;
    const NONCE: u32 = 0x1234_5678;

    #[test]
    fn selects_highest_common_version() {
        assert_eq!(validate_response(CLIENT_MIN, CLIENT_MAX, NONCE,
            1, 2, 2, STATUS_OK, NONCE), Ok(Some(2)));
    }

    #[test]
    fn rejects_nonmaximal_or_out_of_range_selection() {
        assert_eq!(validate_response(CLIENT_MIN, CLIENT_MAX, NONCE,
            1, 2, 1, STATUS_OK, NONCE), Err(NegotiationError::WrongSelection));
        assert_eq!(validate_response(CLIENT_MIN, CLIENT_MAX, NONCE,
            1, 2, 3, STATUS_OK, NONCE), Err(NegotiationError::WrongSelection));
    }

    #[test]
    fn accepts_no_common_only_with_zero_selection() {
        assert_eq!(validate_response(1, 2, NONCE, 3, 4, 0,
            STATUS_NO_COMMON, NONCE), Ok(None));
        assert_eq!(validate_response(1, 2, NONCE, 3, 4, 1,
            STATUS_NO_COMMON, NONCE), Err(NegotiationError::InconsistentStatus));
    }

    #[test]
    fn rejects_ack_nonce_ranges_and_status_mismatches() {
        assert_eq!(validate_response(1, 3, NONCE, 1, 1, 1,
            STATUS_OK, NONCE ^ 1), Err(NegotiationError::NonceMismatch));
        assert_eq!(validate_response(1, 3, NONCE, 0, 1, 1,
            STATUS_OK, NONCE), Err(NegotiationError::InvalidServerRange));
        assert_eq!(validate_response(3, 1, NONCE, 1, 3, 0,
            STATUS_NO_COMMON, NONCE), Err(NegotiationError::InvalidClientRange));
        assert_eq!(validate_response(1, 2, NONCE, 3, 4, 0,
            STATUS_OK, NONCE), Err(NegotiationError::InconsistentStatus));
        assert_eq!(validate_response(1, 2, NONCE, 1, 2, 0,
            STATUS_NO_COMMON, NONCE), Err(NegotiationError::InconsistentStatus));
        assert_eq!(validate_response(1, 2, NONCE, 1, 2, 0,
            STATUS_INVALID_RANGE, NONCE), Err(NegotiationError::InvalidStatus));
        assert_eq!(validate_response(1, 2, NONCE, 1, 2, 0,
            99, NONCE), Err(NegotiationError::InvalidStatus));
    }

    #[test]
    fn legacy_echo_cannot_downgrade_outside_v1_range() {
        assert!(legacy_v1_in_range(1, 3));
        assert!(legacy_v1_in_range(1, 1));
        assert!(!legacy_v1_in_range(2, 3));
        assert!(!legacy_v1_in_range(0, 3));
        assert!(!legacy_v1_in_range(3, 2));
    }
}
