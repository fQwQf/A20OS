/*
 * Smart home hub — frame protocol implementation. See hub_proto.h.
 * Freestanding: no libc; small local helpers instead of string.h so the same
 * source compiles for the target and for the host unit test.
 */
#ifdef CONFIG_BOARD_STM32F103

#include "hub_proto.h"

static size_t hp_strlen(const char *s) {
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

static void hp_copy(uint8_t *dst, const uint8_t *src, size_t n) {
    for (size_t i = 0; i < n; i++)
        dst[i] = src[i];
}

uint16_t hub_crc16(const uint8_t *data, size_t len) {
    /* CRC16-CCITT, poly 0x1021, init 0xFFFF, no reflection. */
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000u)
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            else
                crc = (uint16_t)(crc << 1);
        }
    }
    return crc;
}

int hub_proto_frame(uint8_t type, uint8_t seq, const uint8_t *payload,
                    size_t plen, uint8_t *out, size_t outsize) {
    if (plen > HUB_MAX_PAYLOAD)
        return -1;
    size_t total = HUB_OVERHEAD + plen;
    if (outsize < total)
        return -1;

    out[0] = HUB_MAGIC0;
    out[1] = HUB_MAGIC1;
    out[2] = type;
    out[3] = seq;
    out[4] = (uint8_t)(plen & 0xFFu);
    out[5] = (uint8_t)((plen >> 8) & 0xFFu);
    if (plen)
        hp_copy(out + 6, payload, plen);

    /* CRC covers type..payload = out[2 .. 6+plen). */
    uint16_t crc = hub_crc16(out + 2, 4 + plen);
    out[6 + plen] = (uint8_t)(crc & 0xFFu);
    out[7 + plen] = (uint8_t)((crc >> 8) & 0xFFu);
    return (int)total;
}

int hub_proto_encode_snapshot(uint8_t seq, const env_snapshot_t *s,
                              uint8_t *out, size_t outsize) {
    uint8_t p[5];
    p[0] = (uint8_t)(int8_t)s->temp_c; /* -40..80 fits int8 */
    p[1] = s->humidity;
    p[2] = s->light;
    p[3] = s->hour;
    p[4] = s->valid ? 1u : 0u;
    return hub_proto_frame(HUB_MSG_SNAPSHOT, seq, p, sizeof(p), out, outsize);
}

int hub_proto_encode_control(uint8_t seq, const hub_control_t *c, uint8_t *out,
                             size_t outsize) {
    uint8_t p[5 + HUB_SPEECH_MAX];
    size_t slen = hp_strlen(c->speech);
    if (slen > HUB_SPEECH_MAX)
        slen = HUB_SPEECH_MAX;
    p[0] = c->fan_level;
    p[1] = c->pump_on ? 1u : 0u;
    p[2] = c->theme_id;
    p[3] = c->mood;
    p[4] = (uint8_t)slen;
    hp_copy(p + 5, (const uint8_t *)c->speech, slen);
    return hub_proto_frame(HUB_MSG_CONTROL, seq, p, 5 + slen, out, outsize);
}

int hub_proto_encode_image_req(uint8_t seq, uint8_t theme_id, uint16_t w,
                               uint16_t h, uint8_t *out, size_t outsize) {
    uint8_t p[5];
    p[0] = theme_id;
    p[1] = (uint8_t)(w & 0xFFu);
    p[2] = (uint8_t)((w >> 8) & 0xFFu);
    p[3] = (uint8_t)(h & 0xFFu);
    p[4] = (uint8_t)((h >> 8) & 0xFFu);
    return hub_proto_frame(HUB_MSG_IMAGE_REQ, seq, p, sizeof(p), out, outsize);
}

int hub_proto_decode_image_chunk(const hub_frame_t *f, hub_image_chunk_t *out) {
    if (f->type != HUB_MSG_IMAGE_CHUNK || f->len < HUB_IMAGE_CHUNK_HEADER ||
        !f->payload)
        return -1;
    const uint8_t *p = f->payload;
    out->offset = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                  ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    out->last = (p[4] & HUB_IMAGE_FLAG_LAST) ? 1u : 0u;
    out->data_len = (uint16_t)(f->len - HUB_IMAGE_CHUNK_HEADER);
    out->data = out->data_len ? (p + HUB_IMAGE_CHUNK_HEADER) : (const uint8_t *)0;
    return 0;
}

int hub_proto_parse(const uint8_t *buf, size_t len, hub_frame_t *out) {
    if (len < 2)
        return 0;
    if (buf[0] != HUB_MAGIC0 || buf[1] != HUB_MAGIC1)
        return -1;
    if (len < 6)
        return 0; /* need type/seq/len before we know the size */

    uint16_t plen = (uint16_t)(buf[4] | ((uint16_t)buf[5] << 8));
    if (plen > HUB_MAX_PAYLOAD)
        return -1;
    size_t total = HUB_OVERHEAD + plen;
    if (len < total)
        return 0; /* frame not fully arrived yet */

    uint16_t want = hub_crc16(buf + 2, 4 + plen);
    uint16_t got = (uint16_t)(buf[6 + plen] | ((uint16_t)buf[7 + plen] << 8));
    if (want != got)
        return -1;

    out->type = buf[2];
    out->seq = buf[3];
    out->len = plen;
    out->payload = plen ? (buf + 6) : (const uint8_t *)0;
    return (int)total;
}

int hub_proto_decode_control(const hub_frame_t *f, hub_control_t *out) {
    if (f->type != HUB_MSG_CONTROL || f->len < 5 || !f->payload)
        return -1;
    const uint8_t *p = f->payload;
    uint8_t slen = p[4];
    if ((size_t)5 + slen > f->len)
        return -1;
    if (slen > HUB_SPEECH_MAX)
        slen = HUB_SPEECH_MAX;

    out->fan_level = p[0];
    out->pump_on = p[1] ? 1u : 0u;
    out->theme_id = p[2];
    out->mood = p[3];
    for (uint8_t i = 0; i < slen; i++)
        out->speech[i] = (char)p[5 + i];
    out->speech[slen] = '\0';
    return 0;
}

#endif /* CONFIG_BOARD_STM32F103 */
