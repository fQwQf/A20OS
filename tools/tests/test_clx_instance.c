/* Production wire codecs and membership admission; no shadow state machine. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/include/uapi/a20/cluster_control.h"
static a20_runtime_info_t local = {.size = 64,
                                   .version = 1,
                                   .state = 1,
                                   .instance_id = {1},
                                   .domain_id = {2},
                                   .boot_epoch = 100};
int main(void) {
    assert(a20_control_timing_scale("1") == 1 &&
           a20_control_timing_scale("16") == 16);
    assert(!a20_control_timing_scale("0") && !a20_control_timing_scale("17"));
    assert(!a20_control_timing_scale("4294967296") &&
           !a20_control_timing_scale("-1"));

    a20_runtime_info_t peer = local, out, saved;
    peer.domain_id[0] = 3;
    peer.boot_epoch = 200;
    uint8_t wire[192], old[128], query[16];
    a20_directory_record_t record = {.status = A20_DIRECTORY_END,
                                     .profile = 1,
                                     .generation = 4,
                                     .boot_epoch = 900};
    memcpy(record.node_id, peer.domain_id, 16);
    assert(!a20_directory_runtime_encode(wire, sizeof(wire), &record, &peer));
    a20_directory_record_t decoded = {0}, before;
    assert(!a20_directory_runtime_decode(wire, 192, peer.domain_id, &decoded,
                                         &out));
    assert(out.boot_epoch == 200 && decoded.boot_epoch == 900);
    before = decoded;
    saved = out;
    wire[128 + 56] = 1;
    assert(a20_directory_runtime_decode(wire, 192, peer.domain_id, &decoded,
                                        &out));
    assert(!memcmp(&decoded, &before, sizeof(before)) &&
           !memcmp(&saved, &out, sizeof(out)));
    wire[128 + 56] = 0;
    wire[128 + 32] ^= 1;
    assert(a20_directory_runtime_decode(wire, 192, 0, &decoded, &out));
    a20_directory_encode(old, &record);
    assert(a20_directory_runtime_decode(old, 128, 0, &decoded, &out));
    a20_directory_runtime_request(query, 5, 6);
    uint32_t index;
    uint64_t generation;
    uint8_t version;
    assert(!a20_directory_decode_request_version(query, 16, &index, &generation,
                                                 &version));
    assert(index == 5 && generation == 6 && version == 2);
    assert(a20_directory_decode_request(query, 16, &index, &generation));
    a20_member_t members[2];
    a20_member_service_t services[2];
    a20_membership_t m;
    assert(!a20_membership_init(&m, members, 2, services, 2, 100, 200, 1000));
    assert(!a20_membership_set_runtime(&m, &local));
    int i = a20_member_add(&m, peer.domain_id);
    assert(i >= 0);
    uint64_t cookie = a20_member_probe(&m, i, 1);
    assert(a20_member_observe(&m, i, cookie, 900, 0, 0, 1, 2, 1));
    a20_runtime_info_t bad = peer;
    bad.instance_id[0] = 4;
    assert(a20_member_observe_runtime(&m, i, cookie, 900, 0, 0, 1, 2, 1, &bad));
    bad = (a20_runtime_info_t){.size = 64, .version = 1};
    assert(a20_member_observe_runtime(&m, i, cookie, 900, 0, 0, 1, 2, 1, &bad));
    assert(
        !a20_member_observe_runtime(&m, i, cookie, 900, 0, 0, 1, 2, 1, &peer));
    a20_member_service_t service = {.slot = 1,
                                    .name_len = 4,
                                    .name = "work",
                                    .kinds = A20_WORK_CAP_OPERATOR,
                                    .max_inflight = 1,
                                    .max_input = 12,
                                    .operator_count = 1,
                                    .operators = {{.id = 1, .max_input = 12}}};
    assert(!a20_member_publish(&m, i, &service, 1, 1, 1000, 3));
    assert(a20_member_select(&m, "work", 4, A20_WORK_OPERATOR, 1, 12, 4) >= 0);
    assert(a20_membership_set_runtime(
        &m, &local)); /* cannot activate over populated views */
    a20_member_runtime_reject(&m, i, 5);
    assert(!members[i].runtime_admitted && members[i].runtime_epoch == 200 &&
           !services[0].used);
    cookie = a20_member_probe(&m, i, 6);
    assert(
        a20_member_observe_runtime(&m, i, cookie, 900, 0, 0, 1, 7, 0,
                                   &peer)); /* helpers cannot undo rejection */
    assert(a20_member_observe_runtime(
        &m, i, cookie, 901, 0, 0, 1, 7, 1,
        &peer)); /* new transport boot, reused runtime boot */
    a20_member_runtime_reject(&m, i, 7);
    cookie = a20_member_probe(&m, i, 8);
    assert(a20_member_observe_runtime(
        &m, i, cookie, 901, 0, 0, 1, 9, 1,
        &peer)); /* rejection retained across retry */
    peer.boot_epoch = 201;
    assert(
        !a20_member_observe_runtime(&m, i, cookie, 901, 0, 0, 1, 9, 1, &peer));
    cookie = a20_member_probe(&m, i, 10);
    bad = peer;
    bad.boot_epoch = 202;
    assert(
        a20_member_observe_runtime(&m, i, cookie, 901, 0, 0, 1, 11, 0, &bad));
    assert(
        !a20_member_observe_runtime(&m, i, cookie, 901, 0, 0, 1, 11, 0, &peer));
    assert(!a20_member_runtime(&m, i, &out) && out.boot_epoch == 201);
    uint8_t cm[144] = {'C', 'M', 2, A20_CONTROL_INDIRECT};
    memcpy(cm + 8, peer.domain_id, 16);
    assert(!a20_runtime_info_encode(cm + 80, 64, &peer));
    assert(!a20_control_runtime_decode(cm, 144, &out));
    cm[8] ^= 1;
    saved = out;
    assert(a20_control_runtime_decode(cm, 144, &out));
    assert(!memcmp(&out, &saved, 64));
    assert(!a20_membership_init(&m, members, 2, services, 2, 100, 200, 1000));
    i = a20_member_add(&m, peer.domain_id);
    cookie = a20_member_probe(&m, i, 1);
    assert(!a20_member_observe(&m, i, cookie, 900, 0, 0, 1, 2,
                               1)); /* unchanged legacy policy */
    puts("test_clx_instance: codecs, namespace admission, boot reuse, indirect "
         "proof and legacy PASS");
    return 0;
}
