/*
 * The pure half of NAT: address and port rewrite, checksum fixup, the reply
 * tuple a translated flow is matched on, and which address MASQUERADE takes.
 *
 * Split out of kernel/net/netfilter_nat.c (which includes this) because these
 * are the parts that can be wrong in a way no end-to-end gate can see from the
 * QEMU user-net topology: there is no peer on the far side of a SNAT in a
 * user-mode network, so the address rewrite and the reply-direction match had
 * unit-level evidence and nothing else.  Everything here is a pure function of
 * its arguments and the frame bytes -- no table, no lock, no clock -- which is
 * what makes tools/test-nat-rewrite-host.sh able to compile this header's
 * implementation against the host libc and assert on it directly.
 *
 * Nothing here allocates, and nothing here is reachable from outside the
 * netfilter module except through netfilter_nat.c.
 */
#ifndef _NET_NETFILTER_REWRITE_H
#define _NET_NETFILTER_REWRITE_H

#include "net/netfilter.h"

/* ------------------------------------------------------------------ checksum */

/*
 * Incremental ones-complement update, RFC 1624 equation 3:
 *
 *     HC' = ~(~HC + ~m + m')
 *
 * `old_word` and `new_word` are the 32-bit header words that changed, in wire
 * order.  The sign of each word matters and getting it backwards is silent: the
 * result is a plausible-looking checksum that is wrong by twice the change.
 */
uint16_t netfilter_csum_delta(uint16_t old_csum, uint32_t old_word,
                              uint32_t new_word);

/* Network-byte-order 16-bit access.  Named rather than reusing lwIP's so this
 * file has no lwIP dependency at all. */
uint16_t netfilter_get16(const uint8_t *p);
void netfilter_put16(uint8_t *p, uint16_t v);

/*
 * Offset of the L4 checksum field within the frame, or 0 when it cannot be
 * located inside `len`.
 *
 * UDP's is at a fixed offset 6.  TCP's is at a fixed offset 16 -- *before* the
 * options, not after them -- and the data-offset field is still validated,
 * because a header claiming options the frame does not contain is a header that
 * must not be checksummed at all.
 */
uint16_t netfilter_l4_ckoff(const uint8_t *frame, size_t len,
                            const netfilter_frame_t *pkt);

/* ------------------------------------------------------------------ rewrite */

/* Fix the L4 checksum for the two addresses that moved, and the IP checksum for
 * whichever one moved.  Both are no-ops when nothing changed. */
void netfilter_set_addr(uint8_t *frame, size_t len,
                        const netfilter_frame_t *pkt, uint32_t new_src,
                        uint32_t new_dst);

/* `is_src` selects which of the two ports moves; `new_port` of 0 means "keep the
 * original port", which is how a NAT rule with no toport= is represented. */
void netfilter_set_port(uint8_t *frame, size_t len,
                        const netfilter_frame_t *pkt, int is_src,
                        uint16_t new_port);

/*
 * The address a MASQUERADE translates to: the address of the interface being
 * left, falling back to the primary netif when that one has no IPv4 address
 * yet, so a rule installed before DHCP completes still translates instead of
 * silently passing traffic through untranslated.
 */
uint32_t netfilter_masq_addr(int net_idx);

/* ---------------------------------------------------------------- conntrack */

/*
 * The tuple a reply to `e` arrives with.
 *
 * The entry keeps the tuple as it looked on the wire *before* any translation,
 * so a reply -- which carries the translated values -- is not the reverse of it
 * and cannot be found by the swapped probe.  The reply's source is the forward
 * direction's destination after translation, and its destination is the forward
 * direction's source after translation:
 *
 *   DNAT            reply = (nat_dst_addr:nat_dst_port) -> (src_addr:src_port)
 *   SNAT/MASQUERADE reply = (dst_addr:dst_port) -> (nat_src_addr:nat_src_port)
 *   untranslated    reply = (dst_addr:dst_port) -> (src_addr:src_port)
 *
 * A zero translated port means the rule did not remap the port, so the original
 * is used -- the same convention the rewrite itself follows.
 */
void netfilter_ct_reply_tuple(const net_conntrack_entry_t *e, uint32_t *rsrc,
                              uint32_t *rdst, uint16_t *rsport,
                              uint16_t *rdport);

#endif /* _NET_NETFILTER_REWRITE_H */
