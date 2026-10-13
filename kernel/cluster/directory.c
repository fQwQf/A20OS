/* The discovery endpoint survives user control-plane restarts. It enumerates
 * one published export per call, excluding local-only services. */
#include "cluster/clx_internal.h"
#include "uapi/a20/cluster_directory.h"
#include "proc/proc.h"
#include "core/runtime.h"

void a20_clx_directory_rx(const a20_node_id_t *src, const a20_frame_hdr_t *h,
                          const uint8_t *input) {
    a20_directory_record_t record = {0};
    a20_node_id_t self;
    if (a20_clx_self_get(&self, &record.caps) < 0)
        return;
    memcpy(record.node_id, self.bytes, 16);
    record.profile = CONFIG_CLUSTER_PROFILE;
    record.boot_epoch = a20_clx_boot_session();
    record.status = A20_DIRECTORY_INVALID;
    uint32_t index;
    uint64_t generation;
    uint8_t version = 1;
    if (!a20_directory_decode_request_version(input, h->payload_len, &index,
                                              &generation, &version)) {
        a20_cluster_snapshot_args_t snapshot = {.index = index,
                                                .generation = generation};
        int r;
        do {
            r = a20_clx_export_snapshot(&snapshot);
            snapshot.index = snapshot.next_index;
        } while (!r && (snapshot.flags & A20_EXPORT_LOCAL_ONLY));
        record.generation = snapshot.generation;
        record.status = r == -A20_ERR_BUSY ? A20_DIRECTORY_CHANGED
                        : r < 0            ? A20_DIRECTORY_END
                                           : A20_DIRECTORY_FOUND;
        if (!r) {
            record.next_index = snapshot.next_index;
            record.slot = snapshot.slot;
            record.flags = snapshot.flags;
            record.name_len = snapshot.name_len;
            memcpy(record.name, snapshot.name, record.name_len);
        }
    }
    uint8_t reply[A20_DIRECTORY_RUNTIME_REPLY];
    uint32_t reply_len = A20_DIRECTORY_REPLY;
    if (version == 2) {
        a20_runtime_info_t runtime = {.size = 64, .version = 1};
#ifdef A20_TASK_OBJECTS_ENABLED
        a20_runtime_snapshot(&runtime);
#endif
        if (a20_directory_runtime_encode(reply, sizeof(reply), &record,
                                         &runtime))
            return;
        reply_len = A20_DIRECTORY_RUNTIME_REPLY;
    } else
        a20_directory_encode(reply, &record);
    if (h->flags & A20_CLX_FLAG_RELIABLE) {
        a20_clx_reliable_reply(src, h->txid, h->dst_hash, h->src_session,
                               h->dst_session, reply, reply_len, 0);
    } else {
        uint8_t frame[A20_CLX_HDR_LEN + A20_DIRECTORY_RUNTIME_REPLY +
                      A20_CLX_CRC_LEN];
        a20_frame_hdr_t response = {0};
        response.type = A20_CLX_TYPE_CALL_REPLY;
        response.txid = h->txid;
        response.src_hash = h->dst_hash;
        response.dst_hash = h->src_hash;
        response.dst_slot = A20_DIRECTORY_SLOT;
        response.ttl = A20_CLX_TTL_DEFAULT;
        response.csum_kind = A20_CLX_CSUM_CCITT;
        response.payload_len = reply_len;
        int n = a20_frame_encode(frame, sizeof(frame), sizeof(frame), &response,
                                 reply);
        if (n > 0)
            a20_clx_core_send_frame(src, frame, n);
    }
}
