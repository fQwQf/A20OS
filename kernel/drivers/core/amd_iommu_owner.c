/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CONFIG_MCU
#include "drivers/core/amd_iommu.h"
#include "core/errno.h"

#if defined(CONFIG_X86_64) && defined(CONFIG_ACPI)
#include "drivers/bus/pci_bus.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "drivers/core/dma_mapping.h"
#include "core/klog.h"
#include "mm/slab.h"

#define AMDVI_MIN_REGISTERS 0x4000ULL
#define AMDVI_COUNTER_REGISTERS 0x80000ULL
#define AMDVI_EFR_COUNTERS (1ULL << 9)

/* Passive binding only. No PCI/MMIO writes, DMA queues, IRQs or producers
 * exist yet. A future activation path needs its own revalidation and a
 * fallible hardware-stop protocol before these objects may be reclaimed. */
struct amdvi_unit {
    acpi_ivrs_hardware_t hardware; /* Borrows the retained table. */
    amd_iommu_snapshot_t snapshot;
    resource_t aperture;
    driver_resource_claim_t *claim;
    void *registers;
};

struct amdvi_owner {
    device_t *device; /* Registry/binding and claims protect this reference. */
    acpi_context_t acpi;
    acpi_table_t table;
    size_t count;
    size_t routes; /* Protected with the discovery/lifecycle transaction. */
    bool detached;
    struct amdvi_owner *next;
    struct amdvi_unit units[];
};

static struct amdvi_owner *amdvi_owners;

struct amdvi_peer {
    device_t *device, *scope;
    uint16_t requester;
};

struct amd_iommu_route {
    struct amdvi_owner *owner;
    size_t unit;
    device_t *client, *dma_client, *scope, *dma_scope;
    uint16_t requester;
    pci_requester_reservation_t *reservation;
    struct amdvi_peer *peers;
    size_t peer_count;
};

struct amdvi_transport {
    struct amdvi_owner *owner;
    struct amdvi_unit *unit;
};

static int firmware_error(int status) {
    if (status == ACPI_NOT_FOUND)
        return -ENODEV;
    if (status == ACPI_IVRS_UNSUPPORTED)
        return -EOPNOTSUPP;
    if (status == ACPI_IVRS_AMBIGUOUS)
        return -EBUSY;
    return status == ACPI_UNMAPPED ? -EIO : -EINVAL;
}

static void owner_destroy(struct amdvi_owner *owner) {
    for (size_t i = owner->count; i; i--) {
        struct amdvi_unit *unit = &owner->units[i - 1];
        if (unit->registers)
            arch_device_unmap_mmio(unit->registers, AMDVI_MIN_REGISTERS);
        driver_resource_release(unit->claim);
    }
    acpi_table_release(&owner->acpi, &owner->table);
    kfree(owner);
}

static int owner_pci_read(void *context, uint16_t segment, uint16_t requester,
                          uint16_t offset, uint32_t *out) {
    struct amdvi_transport *transport = context;
    const acpi_ivrs_hardware_t *hardware = &transport->unit->hardware;
    if (segment != hardware->segment || requester != hardware->device_id ||
        segment || (offset & 3) || offset > 252)
        return -EINVAL;
    *out = pci_cfg_read32(transport->owner->device, offset);
    return 0;
}

static int owner_mmio_read(void *context, uint64_t base, uint32_t offset,
                           uint64_t *out) {
    struct amdvi_transport *transport = context;
    struct amdvi_unit *unit = transport->unit;
    if (!unit->claim || base != unit->hardware.base ||
        (offset != 0x18 && offset != 0x30))
        return -EINVAL;
    if (!unit->registers)
        unit->registers = arch_device_map_mmio(base, AMDVI_MIN_REGISTERS, 0);
    if (!unit->registers)
        return -ENOMEM;
    *out = readq((const volatile uint8_t *)unit->registers + offset);
    return 0;
}

/* Generic claims intentionally permit overlapping leases by one owner.
 * Different capabilities in the SAME function must still be disjoint. */
static int unit_claim(struct amdvi_owner *owner, size_t index, uint64_t size) {
    struct amdvi_unit *unit = &owner->units[index];
    uint64_t base = unit->hardware.base;
    if ((base & (size - 1)) || base > ~(uint64_t)0 - (size - 1))
        return -EINVAL;
    resource_t range = {.type = RES_MMIO,
                        .start = base,
                        .end = base + size - 1,
                        .name = "amdvi-registers"};
    for (size_t i = 0; i < index; i++) {
        const resource_t *other = &owner->units[i].aperture;
        if (range.start <= other->end && other->start <= range.end)
            return -EBUSY;
    }
    driver_resource_claim_t *claim;
    int ret = driver_resource_request_checked(owner->device, &range, &claim);
    if (ret)
        return ret;
    /* Acquire the full extent before dropping the minimum-window lease. */
    driver_resource_release(unit->claim);
    unit->claim = claim;
    unit->aperture = range;
    return 0;
}

static int amdvi_probe(device_t *device) {
    unsigned bus, slot, function;
    int ret = pci_device_location(device, &bus, &slot, &function);
    if (ret)
        return ret;
    uint16_t requester = bus << 8 | slot << 3 | function;
    const acpi_context_t *acpi = arch_acpi_context();
    if (!acpi)
        return -ENODEV;
    acpi_table_t table;
    ret = acpi_table_find(acpi, "IVRS", 0, &table);
    if (ret)
        return firmware_error(ret);
    size_t total, count = 0;
    ret = acpi_ivrs_unit_count(&table, &total);
    if (ret)
        goto table_error;
    for (size_t i = 0; i < total; i++) {
        acpi_ivrs_hardware_t hardware;
        ret = acpi_ivrs_unit_get(&table, i, &hardware);
        if (ret)
            goto table_error;
        if (!hardware.segment && hardware.device_id == requester)
            count++;
    }
    if (!count) {
        acpi_table_release(acpi, &table);
        return -ENODEV;
    }
    /* Parser bounds total to 64 units; still validate the allocation math. */
    if (count >
        (~(size_t)0 - sizeof(struct amdvi_owner)) / sizeof(struct amdvi_unit)) {
        acpi_table_release(acpi, &table);
        return -EOVERFLOW;
    }
    struct amdvi_owner *owner =
        kcalloc(1, sizeof(*owner) + count * sizeof(owner->units[0]));
    if (!owner) {
        acpi_table_release(acpi, &table);
        return -ENOMEM;
    }
    owner->device = device;
    owner->acpi = *acpi;
    owner->table = table;
    for (size_t i = 0; i < total; i++) {
        acpi_ivrs_hardware_t hardware;
        ret = acpi_ivrs_unit_get(&owner->table, i, &hardware);
        if (ret) {
            ret = firmware_error(ret);
            goto fail;
        }
        if (hardware.segment || hardware.device_id != requester)
            continue;
        size_t index = owner->count++;
        struct amdvi_unit *unit = &owner->units[index];
        unit->hardware = hardware;
        uint64_t size =
            hardware.type != 0x10 && (hardware.efr & AMDVI_EFR_COUNTERS)
                ? AMDVI_COUNTER_REGISTERS
                : AMDVI_MIN_REGISTERS;
        ret = unit_claim(owner, index, size);
        if (ret)
            goto fail;
        struct amdvi_transport transport = {owner, unit};
        amd_iommu_probe_ops_t ops = {&transport, owner_pci_read,
                                     owner_mmio_read};
        ret = amd_iommu_probe_unit(&hardware, &ops, &unit->snapshot);
        if (ret)
            goto fail;
        if (!unit->snapshot.register_size) {
            /* Disabled legacy IVHD + EFR support: final span is unknown. */
            ret = -EOPNOTSUPP;
            goto fail;
        }
        if (unit->snapshot.register_size != size) {
            ret = unit_claim(owner, index, unit->snapshot.register_size);
            if (ret)
                goto fail;
        }
    }
    device->drv_priv = owner;
    owner->next = amdvi_owners;
    amdvi_owners = owner;
    kinfo(
        "AMDVI_OWNER: bound requester=0x%x units=%lu writes=0 translation=0\n",
        requester, (unsigned long)owner->count);
    return 0;
fail:
    owner_destroy(owner);
    return ret;
table_error:
    acpi_table_release(acpi, &table);
    return firmware_error(ret);
}

static int amdvi_remove(device_t *device) {
    struct amdvi_owner *owner = device->drv_priv;
    device->drv_priv = NULL;
    if (owner) {
        struct amdvi_owner **at = &amdvi_owners;
        while (*at && *at != owner)
            at = &(*at)->next;
        if (*at)
            *at = owner->next;
        owner->detached = true;
        if (!owner->routes)
            owner_destroy(owner);
    }
    return 0;
}

static const device_id_t amdvi_ids[] = {
    {.vendor = 0x1022,
     .device = DEVICE_ANY,
     .subvendor = VENDOR_ANY,
     .subdevice = DEVICE_ANY},
    {0},
};
static driver_t amdvi_driver = {
    .name = "amdvi-owner",
    .id_table = amdvi_ids,
    .bus = &pci_bus,
    .probe = amdvi_probe,
    .remove = amdvi_remove,
    .class_type = DEV_CLASS_NONE,
};
DRIVER_REGISTER(amdvi_driver);

static int route_resolve(struct amdvi_owner *owner, uint16_t requester,
                          acpi_ivrs_route_t *policy, size_t *index) {
    int ret = acpi_ivrs_route(&owner->table, 0, requester, policy);
    if (ret)
        return firmware_error(ret);
    for (size_t i = 0; i < owner->count; i++) {
        const acpi_ivrs_hardware_t *unit = &owner->units[i].hardware;
        if (unit->segment == policy->hardware.segment &&
            unit->device_id == policy->hardware.device_id &&
            unit->capability == policy->hardware.capability &&
            unit->base == policy->hardware.base &&
            unit->type == policy->hardware.type) {
            *index = i;
            return 0;
        }
    }
    return -ENODEV; /* Firmware selected a different controller/capability. */
}

struct route_view {
    amd_iommu_route_info_t info;
    device_t *dma_client, *scope, *dma_scope;
};

static void route_view_release(struct route_view *view) {
    device_put(view->dma_scope);
    device_put(view->scope);
    device_put(view->dma_client);
}

static int route_scope(const amd_iommu_snapshot_t *snapshot, device_t *client,
                        device_t **out) {
    if (snapshot->range_valid)
        return amd_iommu_scope_get(snapshot, client, out);
    unsigned bus, slot, function;
    int ret = pci_device_location(client, &bus, &slot, &function);
    if (ret)
        return ret;
    unsigned devfn = slot << 3 | function;
    /* Firmware supplies scope when RngValid=0. Still verify the actual
     * published client identity and the complete, unambiguous bridge path. */
    return pci_device_scope_get(client, bus, devfn, devfn, out);
}

static int route_observe(struct amdvi_owner *owner, size_t index,
                          device_t *client, const acpi_ivrs_route_t *policy,
                          struct route_view *view) {
    if (owner->detached)
        return -ESHUTDOWN;
    device_t *controller = owner->device;
    if (!device_is_registered(controller) || controller->drv != &amdvi_driver ||
        controller->drv_priv != owner || device_is_stopping(controller))
        return -ENODEV;
    struct amdvi_unit *unit = &owner->units[index];
    struct amdvi_transport transport = {owner, unit};
    amd_iommu_probe_ops_t ops = {&transport, owner_pci_read, owner_mmio_read};
    amd_iommu_snapshot_t now;
    int ret = amd_iommu_probe_unit(&unit->hardware, &ops, &now);
    if (ret)
        return ret;
    const amd_iommu_snapshot_t *before = &unit->snapshot;
    if (now.pci_id != before->pci_id ||
        now.capability_header != before->capability_header ||
        now.range != before->range || now.misc != before->misc ||
        now.register_size != before->register_size ||
        now.mmio_enabled != before->mmio_enabled ||
        now.control != before->control || now.efr != before->efr)
        return -ESTALE;
    ret = route_scope(&now, client, &view->scope);
    if (ret)
        return ret;
    ret = pci_device_find_get(policy->hardware.segment, policy->dma_requester,
                              &view->dma_client);
    if (ret)
        return ret;
    ret = route_scope(&now, view->dma_client, &view->dma_scope);
    if (ret)
        return ret;
    view->info = (amd_iommu_route_info_t){
        .unit = now, .requester = policy->requester,
        .dma_requester = policy->dma_requester, .settings = policy->settings,
        .extended = policy->extended, .memory_count = policy->memory_count,
        .alias = policy->alias, .non_pci_entries = policy->non_pci_entries,
        .hardware_range_checked = now.range_valid,
    };
    return 0;
}

int amd_iommu_route_acquire(device_t *client, amd_iommu_route_t **out) {
    if (!out)
        return -EINVAL;
    *out = NULL;
    if (!client)
        return -EINVAL;
    driver_discovery_begin();
    unsigned bus, slot, function;
    int ret = pci_device_location(client, &bus, &slot, &function);
    if (ret)
        goto done;
    uint16_t requester = bus << 8 | slot << 3 | function;
    struct amdvi_owner *selected = NULL;
    size_t index = 0;
    acpi_ivrs_route_t policy;
    for (struct amdvi_owner *owner = amdvi_owners; owner; owner = owner->next) {
        acpi_ivrs_route_t candidate;
        size_t candidate_index;
        ret = route_resolve(owner, requester, &candidate, &candidate_index);
        if (ret == -ENODEV)
            continue;
        if (ret)
            goto done;
        if (selected) {
            ret = -EBUSY;
            goto done;
        }
        selected = owner;
        index = candidate_index;
        policy = candidate;
    }
    ret = -ENODEV;
    if (!selected)
        goto done;
    struct route_view view = {0};
    ret = route_observe(selected, index, client, &policy, &view);
    if (!ret) {
        struct amd_iommu_route *route = kmalloc(sizeof(*route));
        if (!route)
            ret = -ENOMEM;
        else if (!device_get(client)) {
            kfree(route);
            ret = -ENODEV;
        } else {
            *route = (struct amd_iommu_route){.owner = selected, .unit = index,
                .client = client, .dma_client = view.dma_client,
                .scope = view.scope, .dma_scope = view.dma_scope,
                .requester = requester};
            selected->routes++;
            *out = route;
        }
    }
    if (ret)
        route_view_release(&view);
done:
    driver_discovery_end();
    return ret;
}

static void peers_release(struct amdvi_peer *peers, size_t count) {
    for (size_t i = 0; i < count; i++) {
        device_put(peers[i].scope);
        device_put(peers[i].device);
    }
    kfree(peers);
}

static int peers_validate(const amd_iommu_route_t *route,
                            const amd_iommu_snapshot_t *snapshot) {
    for (size_t i = 0; i < route->peer_count; i++) {
        const struct amdvi_peer *peer = &route->peers[i];
        device_t *device = NULL, *scope = NULL;
        int ret = pci_device_find_get(snapshot->segment, peer->requester, &device);
        if (!ret && device != peer->device) ret = -ESTALE;
        if (!ret && device->user_owned) ret = -EBUSY;
        if (!ret) ret = route_scope(snapshot, device, &scope);
        if (!ret && scope != peer->scope) ret = -ESTALE;
        device_put(scope);
        device_put(device);
        if (ret) return ret;
    }
    return 0;
}

int amd_iommu_route_validate(amd_iommu_route_t *route,
                              amd_iommu_route_info_t *out) {
    if (!out)
        return -EINVAL;
    *out = (amd_iommu_route_info_t){0};
    if (!route)
        return -EINVAL;
    driver_discovery_begin();
    int ret = -ESHUTDOWN;
    struct route_view view = {0};
    if (!route->owner->detached) {
        acpi_ivrs_route_t policy;
        size_t index;
        ret = route_resolve(route->owner, route->requester, &policy, &index);
        if (!ret && index != route->unit)
            ret = -ESTALE;
        if (!ret)
            ret = route_observe(route->owner, index, route->client, &policy, &view);
        if (!ret && (view.dma_client != route->dma_client ||
                     view.scope != route->scope || view.dma_scope != route->dma_scope))
            ret = -ESTALE;
        if (!ret) ret = peers_validate(route, &view.info.unit);
        if (!ret)
            *out = view.info;
    }
    route_view_release(&view);
    driver_discovery_end();
    return ret;
}

int amd_iommu_route_memory(amd_iommu_route_t *route, size_t index,
                            acpi_ivrs_memory_t *out) {
    if (!out)
        return -EINVAL;
    *out = (acpi_ivrs_memory_t){0};
    if (!route)
        return -EINVAL;
    driver_discovery_begin();
    int ret = acpi_ivrs_route_memory(&route->owner->table, 0, route->requester,
                                     index, out);
    driver_discovery_end();
    return ret ? firmware_error(ret) : 0;
}

int amd_iommu_route_requesters(amd_iommu_route_t *route, uint16_t *ids,
                                size_t capacity, size_t *count) {
    if (!count) return -EINVAL;
    *count = 0;
    if (!route || (capacity && !ids)) return -EINVAL;
    driver_discovery_begin();
    acpi_ivrs_requesters_t view;
    int ret = acpi_ivrs_requesters(&route->owner->table, 0, route->requester, &view);
    if (ret) ret = firmware_error(ret);
    else {
        *count = view.count;
        if (capacity < view.count) ret = -ENOSPC;
        else for (size_t i = 0; i < view.count; i++) ids[i] = view.ids[i];
    }
    driver_discovery_end();
    return ret;
}

int amd_iommu_route_reserve(amd_iommu_route_t *route) {
    if (!route)
        return -EINVAL;
    driver_discovery_begin();
    amd_iommu_route_info_t info;
    int ret = amd_iommu_route_validate(route, &info);
    if (ret)
        goto done;
    if (route->reservation) {
        ret = -EALREADY;
        goto done;
    }
    acpi_ivrs_requesters_t view;
    ret = acpi_ivrs_requesters(&route->owner->table, info.unit.segment,
                                route->requester, &view);
    if (ret) { ret = firmware_error(ret); goto done; }
    if (view.count > PCI_REQUESTER_SET_MAX) { ret = -E2BIG; goto done; }
    struct amdvi_peer *peers = kcalloc(view.count, sizeof(*peers));
    if (!peers) { ret = -ENOMEM; goto done; }
    for (size_t i = 0; i < view.count; i++) {
        peers[i].requester = view.ids[i];
        ret = pci_device_find_get(info.unit.segment, view.ids[i], &peers[i].device);
        if (ret) goto discard;
        if (peers[i].device->user_owned) { ret = -EBUSY; goto discard; }
        ret = route_scope(&info.unit, peers[i].device, &peers[i].scope);
        if (ret) goto discard;
        uint32_t command = pci_cfg_read32(peers[i].device, 4);
        if (command == ~(uint32_t)0) { ret = -EIO; goto discard; }
        if (command & 4) { ret = -EBUSY; goto discard; }
    }
    ret = pci_requester_reserve(route->client, info.unit.segment, view.ids,
                                view.count, &route->reservation);
    if (!ret) {
        route->peers = peers;
        route->peer_count = view.count;
        goto done;
    }
discard:
    peers_release(peers, view.count);
done:
    driver_discovery_end();
    return ret;
}

int amd_iommu_route_policy_create(amd_iommu_route_t *route,
                                    struct driver_dma_policy **out) {
    if (!out)
        return -EINVAL;
    *out = NULL;
    if (!route)
        return -EINVAL;
    driver_discovery_begin();
    amd_iommu_route_info_t info;
    int ret = amd_iommu_route_validate(route, &info);
    if (ret)
        goto done;
    acpi_ivrs_requesters_t view;
    ret = acpi_ivrs_requesters(&route->owner->table, info.unit.segment,
                                route->requester, &view);
    if (ret) { ret = firmware_error(ret); goto done; }
    if (view.memory_count > DRIVER_DMA_POLICY_MAX_RULES) {
        ret = -E2BIG;
        goto done;
    }
    struct driver_dma_rule *rules = NULL;
    if (view.memory_count) {
        rules = kmalloc(view.memory_count * sizeof(*rules));
        if (!rules) {
            ret = -ENOMEM;
            goto done;
        }
    }
    for (size_t i = 0; i < view.memory_count; i++) {
        acpi_ivrs_memory_t memory;
        ret = acpi_ivrs_requester_memory(&view, i, &memory);
        if (ret) { ret = firmware_error(ret); goto discard; }
        /* AMD 48882 rev 3.10, table 111: upper bits are reserved;
         * exclusion ignores IR/IW/Unity and requires hardware-wide handling. */
        if (memory.flags & 0xf8) {
            ret = -EOPNOTSUPP;
            goto discard;
        }
        if ((memory.flags & 1) && !(memory.flags & 6)) {
            ret = -EINVAL;
            goto discard;
        }
        rules[i] = (struct driver_dma_rule){memory.start,
            memory.start + memory.length - 1, (memory.flags >> 1) & 3,
            (memory.flags & 1) ? DRIVER_DMA_POLICY_IDENTITY : 0};
    }
    ret = driver_dma_policy_create(rules, view.memory_count, out);
discard:
    kfree(rules);
done:
    driver_discovery_end();
    return ret;
}

void amd_iommu_route_release(amd_iommu_route_t *route) {
    if (!route)
        return;
    driver_discovery_begin();
    struct amdvi_owner *owner = route->owner;
    pci_requester_release(route->reservation);
    peers_release(route->peers, route->peer_count);
    device_put(route->dma_scope);
    device_put(route->scope);
    device_put(route->dma_client);
    device_put(route->client);
    kfree(route);
    if (!--owner->routes && owner->detached)
        owner_destroy(owner);
    driver_discovery_end();
}

int amd_iommu_owner_selftest(void) {
    const acpi_context_t *acpi = arch_acpi_context();
    if (!acpi)
        return -ENODEV;
    acpi_table_t table;
    int ret = acpi_table_find(acpi, "IVRS", 0, &table);
    if (ret)
        return firmware_error(ret);
    /* The IVRS policy reader bounds units to 64. Pin the controllers which
     * were actually bound, so the test never probes unrelated failed devices
     * (a global probe_all could reinitialize storage/network hardware). */
    device_t *devices[64] = {0};
    size_t total = 0, count = 0;
    driver_discovery_begin();
    ret = acpi_ivrs_unit_count(&table, &total);
    if (ret || total > ARRAY_SIZE(devices)) {
        ret = ret ? firmware_error(ret) : -EOVERFLOW;
        goto release_table;
    }
    for (size_t i = 0; i < total; i++) {
        acpi_ivrs_hardware_t hardware;
        ret = acpi_ivrs_unit_get(&table, i, &hardware);
        if (ret) {
            ret = firmware_error(ret);
            goto release_table;
        }
        device_t *found = NULL;
        ret = pci_device_find_get(hardware.segment, hardware.device_id, &found);
        if (ret == -ENODEV || ret == -EOPNOTSUPP)
            continue;
        if (ret)
            goto release_table;
        bool duplicate = found->drv != &amdvi_driver;
        for (size_t j = 0; j < count; j++)
            duplicate |= devices[j] == found;
        if (duplicate)
            device_put(found);
        else
            devices[count++] = found;
    }
    ret = count ? 0 : -ENODEV;
release_table:
    acpi_table_release(acpi, &table);
    if (ret)
        goto done;
    device_t *device = devices[0];
    struct amdvi_owner *owner = device->drv_priv;
    if (!owner || !owner->count) {
        ret = -ENODEV;
        goto done;
    }
    resource_t range = owner->units[0].aperture;
    if (!owner->units[0].snapshot.range_valid) {
        device_t *anchor = (void *)1;
        if (amd_iommu_scope_get(&owner->units[0].snapshot, device, &anchor) !=
                -EOPNOTSUPP || anchor) {
            ret = -EIO;
            goto done;
        }
    }
    device_t peer = {.name = "amdvi-claim-test", .references = 1};
    driver_resource_claim_t *claim = NULL;
    ret = driver_resource_request(&peer, &range, &claim);
    driver_resource_release(claim);
    if (ret != -EBUSY) {
        ret = -EIO;
        goto done;
    }
    ret = driver_unregister(&amdvi_driver);
    if (ret)
        goto done;
    ret = driver_resource_request(&peer, &range, &claim);
    driver_resource_release(claim);
    int rebound = driver_register(&amdvi_driver);
    /* Restore only previous AMD-Vi owners. The outer transaction suppresses
     * register()'s automatic probing of every unbound device. */
    if (!rebound) {
        for (size_t i = 0; i < count; i++)
            if (bus_probe_device(devices[i]) ||
                devices[i]->drv != &amdvi_driver)
                rebound = -EIO;
    }
    if (ret || rebound || device->drv != &amdvi_driver)
        ret = -EIO;
    else
        ret = driver_resource_check_claims(&peer, &range) == -EBUSY ? 0 : -EIO;
done:
    for (size_t i = 0; i < count; i++)
        device_put(devices[i]);
    driver_discovery_end();
    return ret;
}
#else
int amd_iommu_route_requesters(amd_iommu_route_t *route, uint16_t *ids,
                                size_t capacity, size_t *count) {
    (void)route; (void)ids; (void)capacity;
    if (!count) return -EINVAL;
    *count = 0;
    return -EOPNOTSUPP;
}
int amd_iommu_route_policy_create(amd_iommu_route_t *route,
                                    struct driver_dma_policy **out) {
    (void)route;
    if (!out) return -EINVAL;
    *out = NULL;
    return -EOPNOTSUPP;
}
int amd_iommu_route_reserve(amd_iommu_route_t *route) {
    (void)route;
    return -EOPNOTSUPP;
}
int amd_iommu_route_acquire(struct device *client, amd_iommu_route_t **out) {
    (void)client;
    if (!out) return -EINVAL;
    *out = NULL;
    return -EOPNOTSUPP;
}
int amd_iommu_route_validate(amd_iommu_route_t *route, amd_iommu_route_info_t *out) {
    (void)route;
    if (!out) return -EINVAL;
    *out = (amd_iommu_route_info_t){0};
    return -EOPNOTSUPP;
}
int amd_iommu_route_memory(amd_iommu_route_t *route, size_t index,
                            acpi_ivrs_memory_t *out) {
    (void)route; (void)index;
    if (!out) return -EINVAL;
    *out = (acpi_ivrs_memory_t){0};
    return -EOPNOTSUPP;
}
void amd_iommu_route_release(amd_iommu_route_t *route) { (void)route; }
int amd_iommu_owner_selftest(void) {
    return -EOPNOTSUPP;
}
#endif
#endif
