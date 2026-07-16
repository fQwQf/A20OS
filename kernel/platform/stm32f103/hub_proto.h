/*
 * Smart home hub — STM32 <-> cloud-proxy frame protocol (hardware-independent).
 *
 * Frame layout (docs/stm32-big-exp.md §7.1):
 *   0xA5 0x5A | type(1) | seq(1) | len(2, LE) | payload(len) | crc16(2, LE)
 * CRC16-CCITT (poly 0x1021, init 0xFFFF) is computed over
 *   type, seq, len_lo, len_hi, payload  (everything after the two magic bytes,
 *   excluding the crc itself).
 *
 * Pure byte-level codec: testable in QEMU and by the host unit test.
 */
#ifndef _STM32F103_HUB_PROTO_H
#define _STM32F103_HUB_PROTO_H

#include "core/types.h"
#include "env_rule.h"

#define HUB_MAGIC0      0xA5u
#define HUB_MAGIC1      0x5Au
#define HUB_OVERHEAD    8u    /* 2 magic + type + seq + 2 len + 2 crc */
#define HUB_MAX_PAYLOAD 255u
#define HUB_SPEECH_MAX  63u   /* bytes; leaves room for a NUL on decode */

enum hub_msg_type {
    HUB_MSG_HEARTBEAT   = 0x10, /* keepalive                        */
    HUB_MSG_CONTROL     = 0x01, /* downlink: control command        */
    HUB_MSG_SNAPSHOT    = 0x20, /* uplink: environment snapshot     */
    HUB_MSG_IMAGE_REQ   = 0x22, /* uplink: request a background     */
    HUB_MSG_IMAGE_CHUNK = 0x02, /* downlink: one RGB565 image slice */
};

/* Image-chunk payload: offset(4, LE) | flags(1) | rgb565_data(...) (§7.1). */
#define HUB_IMAGE_CHUNK_HEADER 5u
#define HUB_IMAGE_FLAG_LAST    0x01u
#define HUB_IMAGE_MAX_DATA     (HUB_MAX_PAYLOAD - HUB_IMAGE_CHUNK_HEADER)

/* Decoded IMAGE_CHUNK payload — data points into the caller's buffer. */
typedef struct hub_image_chunk {
    uint32_t       offset;   /* byte offset into the full RGB565 image */
    uint8_t        last;     /* 1 on the final chunk                   */
    uint16_t       data_len; /* RGB565 bytes in this chunk             */
    const uint8_t *data;
} hub_image_chunk_t;

/* A parsed frame — payload points into the caller's buffer (no copy). */
typedef struct hub_frame {
    uint8_t        type;
    uint8_t        seq;
    uint16_t       len;
    const uint8_t *payload;
} hub_frame_t;

/* Decoded CONTROL command (downlink from the proxy, §7.2). */
typedef struct hub_control {
    uint8_t fan_level;             /* 0..3                                   */
    uint8_t pump_on;               /* 0/1                                    */
    uint8_t theme_id;              /* env_theme_t                            */
    uint8_t mood;                  /* Live2D mood id                         */
    char    speech[HUB_SPEECH_MAX + 1]; /* NUL-terminated bubble text        */
} hub_control_t;

uint16_t hub_crc16(const uint8_t *data, size_t len);

/* Build a frame from a raw payload. Returns total bytes written, or -1 on
 * overflow / oversized payload. */
int hub_proto_frame(uint8_t type, uint8_t seq, const uint8_t *payload,
                    size_t plen, uint8_t *out, size_t outsize);

/* Encode an environment snapshot as a SNAPSHOT frame. */
int hub_proto_encode_snapshot(uint8_t seq, const env_snapshot_t *s,
                              uint8_t *out, size_t outsize);

/* Encode a control command as a CONTROL frame. */
int hub_proto_encode_control(uint8_t seq, const hub_control_t *c, uint8_t *out,
                             size_t outsize);

/* Validate and parse one frame at buf[0..len).
 *   > 0 : success, returns the number of bytes the frame occupies
 *     0 : incomplete — need more bytes before the frame is whole
 *    -1 : bad magic or CRC mismatch (caller should resync/drop) */
int hub_proto_parse(const uint8_t *buf, size_t len, hub_frame_t *out);

/* Decode a CONTROL frame's payload into hub_control_t. Returns 0, or -1 if
 * the frame is not a CONTROL frame or is malformed. */
int hub_proto_decode_control(const hub_frame_t *f, hub_control_t *out);

/* Encode an IMAGE_REQ frame (request the proxy render a w*h background for the
 * given theme). Returns total bytes written, or -1 on overflow. */
int hub_proto_encode_image_req(uint8_t seq, uint8_t theme_id, uint16_t w,
                               uint16_t h, uint8_t *out, size_t outsize);

/* Decode an IMAGE_CHUNK frame into hub_image_chunk_t (data aliases the frame's
 * payload). Returns 0, or -1 if not an IMAGE_CHUNK frame / malformed. */
int hub_proto_decode_image_chunk(const hub_frame_t *f, hub_image_chunk_t *out);

#endif /* _STM32F103_HUB_PROTO_H */
