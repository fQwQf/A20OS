/*
 * refdec.c -- host-side reference decoder for the A20OS cluster wire format.
 *
 * This file exists so the C side of the diff has a concrete, compilable
 * definition of the output format that check_c_side.py parses, and so the
 * Python reference in clframe.py has an independent second implementation to be
 * compared against on the host before anyone ports it to kernel/cluster/frame.c.
 *
 * Style constraints mirrored from docs/cluster/03-kernel-impl.md 2 on purpose:
 * explicit per-field reads, no struct image on the wire, no allocation, no
 * locks.  Everything below is a plain function over a caller-supplied buffer.
 *
 * Usage:
 *   cc -O2 -Wall -Wextra -o refdec refdec.c
 *   ./refdec <file.hex> [<mtu>]      # mtu default 1472 (UDP)
 *   ./refdec --selftest <file.hex>
 *
 * Output is one "key=value" per line on stdout, as specified in README.md.
 * Exit status: 0 when the frame was accepted, 1 when it was rejected with a
 * verdict (which is a *correct* outcome for a malformed vector, so read
 * "reason=" rather than the exit status), 2 on a usage error.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- 02-wire-protocol.md 1: header layout, multi-byte fields little-endian - */
#define CL_MAGIC        0x4C43u
#define CL_VER          0u
#define CL_HEADER_LEN   32u
#define CL_OFF_MAGIC    0u
#define CL_OFF_VER      2u
#define CL_OFF_TYPE     3u
#define CL_OFF_FLAGS    4u
#define CL_OFF_FRAG     6u
#define CL_OFF_TXID     8u
#define CL_OFF_SRC      12u
#define CL_OFF_DST      16u
#define CL_OFF_SLOT     20u
#define CL_OFF_SEQ      24u
#define CL_OFF_PLEN     28u
#define CL_OFF_TTL      30u
#define CL_OFF_CSUM     31u
#define CL_OFF_PAYLOAD  32u

/* ---- 02-2: message types ------------------------------------------------ */
enum {
	CL_T_HELLO = 1, CL_T_HELLO_ACK = 2, CL_T_PING = 3, CL_T_PONG = 4,
	CL_T_SEND = 5, CL_T_CALL = 6, CL_T_CALL_REPLY = 7, CL_T_CLOSE = 8,
	CL_T_ACK = 9, CL_T_NACK = 10, CL_T_ERROR = 11
};

/* ---- 02-3: flags -------------------------------------------------------- */
#define CL_F_RELIABLE    0x0001u
#define CL_F_FRAGMENTED  0x0002u
#define CL_F_BROADCAST   0x0004u
#define CL_F_COMPRESSED  0x0008u
#define CL_F_KNOWN       0x000Fu

/* ---- 02-5: frag field --------------------------------------------------- */
#define CL_FRAG_SEQ_MASK   0x0FFFu
#define CL_FRAG_LAST       0x8000u
#define CL_FRAG_RESERVED   0x7000u

/* ---- 02-1: csum_kind ---------------------------------------------------- */
#define CL_CSUM_NONE   0u
#define CL_CSUM_CCITT  1u

/* ---- 02-10 verdicts, identical to the strings in clframe.py -------------- */
#define R_OK                     "OK"
#define R_SHORT                  "short_frame"
#define R_MAGIC                  "bad_magic"
#define R_VER                    "unsupported_ver"
#define R_TYPE                   "unknown_type"
#define R_LEN                    "payload_len_mismatch"
#define R_OVERFLOW               "payload_len_over_mtu"
#define R_CSUM_KIND              "unsupported_csum_kind"
#define R_CRC                    "crc_mismatch"
#define R_FRAG                   "frag_flag_mismatch"
#define R_SEQ                    "seq_on_unreliable_frame"
#define R_TXID                   "txid_on_send"
#define R_FIXED_LEN              "fixed_payload_len_mismatch"

/* --------------------------------------------------------------------- */

struct cl_result {
	int         accept;
	const char *reason;
	uint32_t    magic;
	uint8_t     ver;
	uint8_t     type;
	const char *type_name;
	uint16_t    flags;
	uint16_t    frag;
	uint32_t    txid;
	uint32_t    src_hash;
	uint32_t    dst_hash;
	uint32_t    dst_slot;
	uint32_t    seq;
	uint16_t    payload_len;
	uint8_t     ttl;
	uint8_t     csum_kind;
	int         crc_present;
	uint16_t    crc_value;
	const uint8_t *payload;
};

/* explicit little-endian readers: no struct image, no alignment assumption */
static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t rd32_at(const uint8_t *p, unsigned off)
{
	return rd32(p + off);
}

/* 02-1 csum_kind=1: CRC16-CCITT poly 0x1021 init 0xFFFF, no reflection, no
 * final xor (the CRC-16/CCITT-FALSE parameterisation). */
static uint16_t crc16_ccitt(const uint8_t *data, uint32_t len)
{
	uint32_t i;
	uint16_t crc = 0xFFFFu;
	int b;

	for (i = 0; i < len; i++) {
		crc ^= (uint16_t)((uint16_t)data[i] << 8);
		for (b = 0; b < 8; b++) {
			if (crc & 0x8000u)
				crc = (uint16_t)((uint16_t)(crc << 1) ^ 0x1021u);
			else
				crc = (uint16_t)(crc << 1);
		}
	}
	return crc;
}

static const char *type_name(uint8_t t)
{
	switch (t) {
	case CL_T_HELLO: return "HELLO";
	case CL_T_HELLO_ACK: return "HELLO_ACK";
	case CL_T_PING: return "PING";
	case CL_T_PONG: return "PONG";
	case CL_T_SEND: return "SEND";
	case CL_T_CALL: return "CALL";
	case CL_T_CALL_REPLY: return "CALL_REPLY";
	case CL_T_CLOSE: return "CLOSE";
	case CL_T_ACK: return "ACK";
	case CL_T_NACK: return "NACK";
	case CL_T_ERROR: return "ERROR";
	default: return "UNKNOWN";
	}
}

/* 02-2 payload column; -1 means variable length */
static int fixed_payload_len(uint8_t t)
{
	switch (t) {
	case CL_T_HELLO:
	case CL_T_HELLO_ACK: return 32;	/* 02-6 */
	case CL_T_PING:
	case CL_T_PONG: return 8;
	case CL_T_SEND:
	case CL_T_CALL:
	case CL_T_CALL_REPLY: return -1;
	case CL_T_CLOSE: return 0;
	case CL_T_ACK:
	case CL_T_NACK: return 4;
	case CL_T_ERROR: return 8;
	default: return -2;
	}
}

/*
 * The layer-1 decoder.  Same contract as clframe.decode_frame(): the receiving
 * transport's MTU is an input, because 02-1 bounds payload_len by it.
 */
static void decode_frame(const uint8_t *buf, uint32_t len, uint32_t mtu,
			 struct cl_result *out)
{
	uint32_t max_plen;
	uint32_t crc_len;
	int fixed;
	uint16_t got, want;

	memset(out, 0, sizeof(*out));
	out->reason = R_OK;
	out->type_name = "UNKNOWN";

	if (len < CL_HEADER_LEN) {
		out->reason = R_SHORT;
		return;
	}

	out->magic       = rd16(buf + CL_OFF_MAGIC);
	out->ver         = buf[CL_OFF_VER];
	out->type        = buf[CL_OFF_TYPE];
	out->flags       = rd16(buf + CL_OFF_FLAGS);
	out->frag        = rd16(buf + CL_OFF_FRAG);
	out->txid        = rd32_at(buf, CL_OFF_TXID);
	out->src_hash    = rd32_at(buf, CL_OFF_SRC);
	out->dst_hash    = rd32_at(buf, CL_OFF_DST);
	out->dst_slot    = rd32_at(buf, CL_OFF_SLOT);
	out->seq         = rd32_at(buf, CL_OFF_SEQ);
	out->payload_len = rd16(buf + CL_OFF_PLEN);
	out->ttl         = buf[CL_OFF_TTL];
	out->csum_kind   = buf[CL_OFF_CSUM];
	out->type_name   = type_name(out->type);

	if (out->magic != CL_MAGIC) {
		out->reason = R_MAGIC;
		return;
	}
	/* 02-1: magic/ver mismatch drops the frame; a higher ver is handled in the
	 * HELLO phase, data frames are dropped silently. */
	if (out->ver != CL_VER) {
		out->reason = R_VER;
		return;
	}
	if (out->type_name[0] == 'U' && out->type == 0) {
		out->reason = R_TYPE;
		return;
	}
	{
		const char *n = type_name(out->type);
		uint8_t t = out->type;
		int known = 0;

		(void)n;
		for (int k = CL_T_HELLO; k <= CL_T_ERROR; k++)
			if (t == (uint8_t)k)
				known = 1;
		if (!known) {
			out->reason = R_TYPE;
			return;
		}
	}
	if (out->csum_kind != CL_CSUM_NONE && out->csum_kind != CL_CSUM_CCITT) {
		out->reason = R_CSUM_KIND;
		return;
	}

	/* 02-1: payload_len <= transport MTU - header - CRC.  The 2 CRC bytes are
	 * subtracted unconditionally, exactly as the sentence reads. */
	if (mtu < CL_HEADER_LEN + 2u) {
		out->reason = R_OVERFLOW;
		return;
	}
	max_plen = mtu - CL_HEADER_LEN - 2u;
	if (out->payload_len > max_plen) {
		out->reason = R_OVERFLOW;
		return;
	}

	out->crc_present = (out->csum_kind == CL_CSUM_CCITT);
	crc_len = out->crc_present ? 2u : 0u;

	/* 02-1: payload_len must match what the transport actually received. */
	if (len != (uint32_t)CL_HEADER_LEN + out->payload_len + crc_len) {
		out->reason = R_LEN;
		return;
	}

	/* From here on the payload bytes are in bounds regardless of the final
	 * verdict (frag/seq/txid/fixed-length rejects still have a readable
	 * payload), so the printer below can always emit payload_hex. */
	out->payload = buf + CL_OFF_PAYLOAD;

	if (out->crc_present) {
		got = rd16(buf + CL_OFF_PAYLOAD + out->payload_len);
		want = crc16_ccitt(buf, (uint32_t)CL_HEADER_LEN + out->payload_len);
		out->crc_value = got;
		if (got != want) {
			/* the CRC covers the 32-byte header as well as the payload */
			out->reason = R_CRC;
			return;
		}
	}

	/* 02-3/02-5: the FRAGMENTED flag and the frag field must agree.  A message
	 * that fits in one frame is not fragmented (assumption A-04). */
	if (out->flags & CL_F_FRAGMENTED) {
		if ((out->frag & CL_FRAG_SEQ_MASK) == 0 &&
		    (out->frag & CL_FRAG_LAST)) {
			out->reason = R_FRAG;
			return;
		}
	} else if (out->frag != 0) {
		out->reason = R_FRAG;
		return;
	}

	/* 02-1: seq is the reliable-tier number; an unreliable frame puts 0. */
	if (!(out->flags & CL_F_RELIABLE) && out->seq != 0) {
		out->reason = R_SEQ;
		return;
	}
	/* 02-1: SEND puts txid 0. */
	if (out->type == CL_T_SEND && out->txid != 0) {
		out->reason = R_TXID;
		return;
	}
	/* 02-2/02-6: fixed payload sizes. */
	fixed = fixed_payload_len(out->type);
	if (fixed >= 0 && (int)out->payload_len != fixed) {
		out->reason = R_FIXED_LEN;
		return;
	}

	out->accept = 1;
}

/* ---- 04-4: SLIP variant ------------------------------------------------- */
#define SLIP_END  0xC0u
#define SLIP_ESC  0xDBu
#define SLIP_ESC_END 0xDCu
#define SLIP_ESC_ESC 0xDDu
/* 04-4: the leaf owns a single static 512 B receive buffer; a half frame that
 * no longer fits is dropped and the receiver resyncs on the next END. */
#define CL_SLIP_RX_BUF 512u

static uint32_t slip_encode(const uint8_t *in, uint32_t len, uint8_t *out)
{
	uint32_t i, n = 0;

	out[n++] = SLIP_END;
	for (i = 0; i < len; i++) {
		if (in[i] == SLIP_END) {
			out[n++] = SLIP_ESC;
			out[n++] = SLIP_ESC_END;
		} else if (in[i] == SLIP_ESC) {
			out[n++] = SLIP_ESC;
			out[n++] = SLIP_ESC_ESC;
		} else {
			out[n++] = in[i];
		}
	}
	out[n++] = SLIP_END;
	return n;
}

/* returns the length of the first complete frame; 0 with *note set otherwise.
 * The authoritative multi-frame decoder is slip_decode_all(); this one exists so
 * the file can check its own encode/decode pair. */
static uint32_t slip_decode(const uint8_t *in, uint32_t len, uint8_t *out,
			    uint32_t max, const char **note)
{
	uint32_t i = 0, n = 0;
	int started = 0;

	*note = "OK";
	while (i < len) {
		uint8_t b = in[i++];

		if (b == SLIP_END) {
			if (started && n == 0)
				*note = "empty";	/* resync artifact */
			else if (started && n > 0)
				return n;		/* one frame complete */
			started = 1;
			n = 0;
			continue;
		}
		started = 1;
		if (b == SLIP_ESC) {
			uint8_t nx;

			if (i >= len) {
				*note = "unterminated_escape";
				return 0;
			}
			nx = in[i++];
			if (nx == SLIP_ESC_END)
				b = SLIP_END;
			else if (nx == SLIP_ESC_ESC)
				b = SLIP_ESC;
			else
				*note = "bad_escape";
		}
		if (n >= max) {
			*note = "oversize";
			return 0;
		}
		out[n++] = b;
	}
	if (started && n)
		*note = "partial_frame";
	return 0;
}

/*
 * 04-4 full decoder: collects every complete frame into a flat buffer, records
 * each frame's length in lens[], and records one letter per 04-4 diagnostic in
 * notes[] ('e' empty/resync artifact, 'u' unterminated escape, 'x' bad escape
 * code, 'o' over the receive buffer, 'p' partial frame at end of stream,
 * 't' END-terminated but shorter than the 32 B header, 'm' bad magic after
 * unescaping -- both dropped, never handed to the CL layer).
 * Semantics match clframe.slip_decode(), which returns the same frames and the
 * same note names as words:
 *   - a bad escape code consumes the pair and emits NO byte;
 *   - a half frame that exceeds `cap` is dropped at the moment it no longer
 *     fits and every byte up to the next END is ignored ('o' once);
 *   - `cap` is the 512 B receive buffer of 04-4; `arena` bounds the flat
 *     output, and neither is written past.
 */
static uint32_t slip_decode_all(const uint8_t *in, uint32_t len,
				uint8_t *out, uint32_t arena, uint32_t cap,
				uint32_t *lens, uint32_t max_frames,
				char *notes, uint32_t notes_cap)
{
	uint8_t cur[512];
	uint32_t i = 0, n = 0, off = 0, frames = 0, used = 0;
	int started = 0, skipping = 0, dropped = 0;

	while (i < len && used + 1 < notes_cap) {
		uint8_t b = in[i++];

		if (b == SLIP_END) {
			if (started && n == 0 && !dropped) {
				notes[used++] = 'e';	/* resync artifact */
			} else if (started && n > 0) {
				uint32_t k;

				/* clframe.slip_decode() finish(): a short or
				 * wrongly-magicked frame is a note, not a
				 * frame -- the CL layer never sees it. */
				if (n < CL_HEADER_LEN) {
					notes[used++] = 't';
				} else if (cur[0] != (CL_MAGIC & 0xFFu) ||
					   cur[1] != (CL_MAGIC >> 8)) {
					notes[used++] = 'm';
				} else {
					if (frames < max_frames)
						lens[frames] = n;
					frames++;
					for (k = 0; k < n && off < arena; k++)
						out[off++] = cur[k];
				}
			}
			n = 0;
			started = 1;
			skipping = 0;
			dropped = 0;
			continue;
		}
		if (skipping)
			continue;	/* dropping the oversize half frame */
		started = 1;
		if (b == SLIP_ESC) {
			uint8_t nx;

			if (i >= len) {
				notes[used++] = 'u';
				break;
			}
			nx = in[i++];
			if (nx == SLIP_ESC_END)
				b = SLIP_END;
			else if (nx == SLIP_ESC_ESC)
				b = SLIP_ESC;
			else {
				notes[used++] = 'x';
				continue;	/* no byte is emitted */
			}
		}
		if (n >= cap) {
			/* 04-4: the half frame does not fit the static receive
			 * buffer; drop it and resync on the next END */
			notes[used++] = 'o';
			n = 0;
			skipping = 1;
			dropped = 1;	/* the terminating END is the resync point */
			continue;
		}
		cur[n++] = b;
	}
	if (started && n)
		notes[used++] = 'p';
	notes[used] = '\0';
	return frames;
}

/* ---- output ------------------------------------------------------------- */

static void emit_kv_u32(const char *key, uint32_t v, const char *fmt)
{
	printf("%s=", key);
	printf(fmt, v);
	printf("\n");
}

static void emit_frame(uint32_t len, const struct cl_result *r, uint32_t mtu)
{
	uint32_t i;

	printf("wire_len=%u\n", len);
	printf("magic=0x%04X\n", r->magic);
	printf("ver=%u\n", r->ver);
	printf("type=%u\n", r->type);
	printf("type_name=%s\n", r->type_name);
	printf("flags=0x%04X\n", r->flags);
	printf("flags_reliable=%u\n", !!(r->flags & CL_F_RELIABLE));
	printf("flags_fragmented=%u\n", !!(r->flags & CL_F_FRAGMENTED));
	printf("flags_broadcast=%u\n", !!(r->flags & CL_F_BROADCAST));
	printf("flags_compressed=%u\n", !!(r->flags & CL_F_COMPRESSED));
	printf("flags_reserved_nonzero=%u\n",
	       !!(r->flags & (uint16_t)~CL_F_KNOWN));
	printf("frag=0x%04X\n", r->frag);
	printf("frag_seq=%u\n", r->frag & CL_FRAG_SEQ_MASK);
	printf("frag_last=%u\n", !!(r->frag & CL_FRAG_LAST));
	printf("frag_reserved_nonzero=%u\n", !!(r->frag & CL_FRAG_RESERVED));
	printf("txid=%u\n", r->txid);
	printf("src_hash=0x%08X\n", r->src_hash);
	printf("dst_hash=0x%08X\n", r->dst_hash);
	printf("dst_slot=%u\n", r->dst_slot);
	printf("seq=%u\n", r->seq);
	printf("payload_len=%u\n", r->payload_len);
	printf("ttl=%u\n", r->ttl);
	printf("csum_kind=%u\n", r->csum_kind);
	printf("crc_present=%u\n", (unsigned)r->crc_present);
	if (r->crc_present)
		printf("crc_value=0x%04X\n", r->crc_value);
	else
		printf("crc_value=-\n");
	printf("crc_ok=%u\n", r->accept && r->crc_present ? 1u :
	       (!r->crc_present ? 1u : 0u));
	printf("payload_hex=");
	if (r->payload)
		for (i = 0; i < r->payload_len; i++)
			printf("%02x", r->payload[i]);
	printf("\n");

	/* optional keys: layer-2 and HELLO interpretation.  check_c_side.py
	 * compares them only when they are present (see README). */
	if (r->accept && (r->type == CL_T_SEND || r->type == CL_T_CALL ||
			  r->type == CL_T_CALL_REPLY) &&
	    !(r->flags & CL_F_FRAGMENTED) && r->payload_len >= 4) {
		printf("ack_prefix=0x%08X\n", rd32(r->payload));
		printf("app_payload_len=%u\n", r->payload_len - 4u);
		printf("app_payload_hex=");
		for (i = 4; i < r->payload_len; i++)
			printf("%02x", r->payload[i]);
		printf("\n");
	}
	if (r->accept && (r->type == CL_T_HELLO || r->type == CL_T_HELLO_ACK)) {
		const uint8_t *h = r->payload;
		uint32_t hash = 2166136261u;
		char hex[33];

		/* 01-abi: FNV-1a over the node id */
		for (i = 0; i < 16u; i++) {
			hash ^= h[i];
			hash *= 16777619u;
		}
		for (i = 0; i < 16u; i++)
			sprintf(hex + i * 2, "%02x", h[i]);
		hex[32] = '\0';
		printf("hello_node_id=%s\n", hex);
		printf("hello_node_id_hash=0x%08X\n", hash);
		printf("hello_proto_min=%u\n", h[16]);
		printf("hello_proto_max=%u\n", h[17]);
		printf("hello_profile_tier=%u\n", h[18]);
		printf("hello_caps=0x%02X\n", h[19]);
		printf("hello_short_addr=0x%04X\n", rd16(h + 20));
		printf("hello_link_addr_len=%u\n", h[22]);
		printf("hello_reserved=%u\n", h[23]);
		for (i = 0; i < 8u; i++)
			sprintf(hex + i * 2, "%02x", h[24 + i]);
		hex[16] = '\0';
		printf("hello_nonce=%s\n", hex);
	}
	printf("transport_mtu=%u\n", mtu);
	emit_kv_u32("accept", (uint32_t)r->accept, "%u");
	printf("reason=%s\n", r->reason);
}

static uint32_t read_hex_file(const char *path, uint8_t *out, uint32_t cap)
{
	FILE *f = fopen(path, "r");
	static char line[65536];
	uint32_t n = 0;
	uint32_t i;
	int hi, lo;

	if (!f) {
		fprintf(stderr, "refdec: cannot open %s\n", path);
		return 0;
	}
	if (!fgets(line, sizeof(line), f)) {
		fclose(f);
		return 0;
	}
	fclose(f);
	for (i = 0; line[i] && line[i] != '\n' && line[i] != '\r'; i++) {
		if (sscanf(line + i, "%1x%1x", &hi, &lo) != 2)
			continue;
		if (n >= cap)
			break;
		out[n++] = (uint8_t)((hi << 4) | lo);
		i++;
	}
	return n;
}

/* 04-4 SLIP mode: prints frames=<n>, frame<i>_hex=... and notes=<letters> so
 * check_c_side.py --mode slip can diff a C SLIP decoder against the vectors. */
static int run_slip(const char *path)
{
	uint8_t in[8192];
	uint8_t out[8192];
	uint32_t lens[64];
	char notes[256];
	uint32_t len, frames, off = 0, i;

	len = read_hex_file(path, in, sizeof(in));
	if (len == 0) {
		fprintf(stderr, "refdec: no bytes read from %s\n", path);
		return 2;
	}
	frames = slip_decode_all(in, len, out, sizeof(out), CL_SLIP_RX_BUF,
				 lens, 64, notes, sizeof(notes));
	printf("stream_len=%u\n", len);
	printf("frames=%u\n", frames);
	for (i = 0; i < frames; i++) {
		uint32_t k;

		printf("frame%u_hex=", i);
		for (k = 0; k < lens[i]; k++)
			printf("%02x", out[off + k]);
		printf("\n");
		off += lens[i];
	}
	printf("notes=%s\n", notes[0] ? notes : "-");
	return 0;
}

static void selftest(void)
{
	struct cl_result r;
	uint8_t buf[2048];
	uint32_t len;
	static const uint8_t zeros[16] = { 0 };
	static const uint8_t ones[16] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
	};
	uint32_t h;
	uint8_t esc[2048];
	uint8_t back[2048];
	const char *note;
	uint32_t n;

	/* 01-abi: FNV-1a32 anchors already fixed by W0 */
	h = 2166136261u;
	for (int i = 0; i < 16; i++) {
		h ^= zeros[i];
		h *= 16777619u;
	}
	printf("fnv_zero=0x%08X expect=0x69691905 %s\n", h,
	       h == 0x69691905u ? "ok" : "FAIL");
	h = 2166136261u;
	for (int i = 0; i < 16; i++) {
		h ^= ones[i];
		h *= 16777619u;
	}
	printf("fnv_ff=0x%08X expect=0x360779f5 %s\n", h,
	       h == 0x360779f5u ? "ok" : "FAIL");

	/* 02-1: CRC-16/CCITT-FALSE check value */
	printf("crc_check=0x%04X expect=0x29B1 %s\n",
	       crc16_ccitt((const uint8_t *)"123456789", 9),
	       crc16_ccitt((const uint8_t *)"123456789", 9) == 0x29B1u ?
	       "ok" : "FAIL");

	/* 04-4: SLIP round trip through this file's own helpers */
	memset(buf, 0x5a, sizeof(buf));
	buf[0] = 0x43; buf[1] = 0x4c;
	len = slip_encode(buf, 64, esc);
	n = slip_decode(esc, len, back, sizeof(back), &note);
	printf("slip_roundtrip=%s\n",
	       (n == 64 && memcmp(buf, back, 64) == 0) ? "ok" : "FAIL");

	/* 04-4: escapes, two back-to-back frames, and an empty frame between ENDs */
	memset(buf, 0xdb, 64);
	len = slip_encode(buf, 64, esc);
	{
		uint32_t m = slip_encode(buf, 32, esc + len);

		len += m;
		esc[len++] = SLIP_END;	/* an empty frame before the second */
	}
	n = slip_decode(esc, len, back, sizeof(back), &note);
	printf("slip_escape_roundtrip=%s\n",
	       (n == 64 && memcmp(buf, back, 64) == 0) ? "ok" : "FAIL");

	/* 02-1: a truncated buffer must be rejected, never read past */
	decode_frame(buf, 20, 1472, &r);
	printf("short_frame=%s\n", r.accept ? "FAIL" :
	       strcmp(r.reason, R_SHORT) == 0 ? "ok" : "FAIL");
}

int main(int argc, char **argv)
{
	uint8_t buf[4096];
	uint32_t len;
	uint32_t mtu = 1472;
	struct cl_result r;
	const char *path;

	if (argc > 1 && strcmp(argv[1], "--selftest") == 0) {
		selftest();
		return 0;
	}
	if (argc > 1 && strcmp(argv[1], "--slip") == 0) {
		if (argc < 3) {
			fprintf(stderr, "usage: %s --slip <file.slip.hex>\n", argv[0]);
			return 2;
		}
		return run_slip(argv[2]);
	}
	if (argc < 2) {
		fprintf(stderr,
			"usage: %s <file.hex> [mtu]\n"
			"       %s --slip <file.slip.hex>\n"
			"       %s --selftest\n", argv[0], argv[0], argv[0]);
		return 2;
	}
	path = argv[1];
	if (argc > 2)
		mtu = (uint32_t)strtoul(argv[2], NULL, 0);

	len = read_hex_file(path, buf, sizeof(buf));
	if (len == 0) {
		fprintf(stderr, "refdec: no bytes read from %s\n", path);
		return 2;
	}
	decode_frame(buf, len, mtu, &r);
	emit_frame(len, &r, mtu);
	return r.accept ? 0 : 1;
}
