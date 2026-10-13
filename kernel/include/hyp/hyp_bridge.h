#ifndef A20_HYP_BRIDGE_H
#define A20_HYP_BRIDGE_H
/* Process exit detaches slot ownership while tasklist_lock still prevents
 * tgid reuse, then releases references outside every task-list/slot lock. */
#define HYP_BRIDGE_SLOTS 128
struct hyp_bridge_drop {
    void *objects[HYP_BRIDGE_SLOTS];
    unsigned char kinds[HYP_BRIDGE_SLOTS];
    unsigned count;
};
void hyp_bridge_detach_group(int tgid, struct hyp_bridge_drop *drop);
void hyp_bridge_release_group(struct hyp_bridge_drop *drop);
#endif
