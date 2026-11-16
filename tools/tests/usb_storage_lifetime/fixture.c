/* Real storage/core/USB ownership; controller commands and DMA are modeled. */
#include "drivers/usb/class/usb_storage.c"

static unsigned storage_configures, storage_releases, storage_interfaces;
static unsigned storage_owned, storage_phase, storage_transferred;
static int storage_configure_fail, storage_release_fail, storage_stop_in_io;
static int storage_control_fail, storage_bulk_fail_stage, storage_csw_fault;
static unsigned storage_cbws, storage_trace_len;
static char storage_trace[32];
static usb_msc_cbw_t storage_cbw;

static void storage_record(char step) {
    assert(storage_trace_len + 1 < sizeof(storage_trace));
    storage_trace[storage_trace_len++] = step;
    storage_trace[storage_trace_len] = 0;
}

static int storage_control(usb_hcd_t *h, uint8_t slot,
                           const usb_setup_packet_t *setup, void *data) {
    assert(slot == 1 && !data && !setup->length);
    if (setup->request == USB_REQ_SET_INTERFACE) {
        assert(setup->request_type == (USB_TYPE_STANDARD | USB_RECIP_INTERFACE));
        storage_interfaces++;
        return 0;
    }
    usb_storage_dev_t *st = h->port_devices[0]->ifaces[0].device->drv_priv;
    assert(st && st->lock.held && st->recovery_pending && !storage_owned);
    assert(g_driver_core_ops.held && !setup->value);
    int stage;
    if (setup->request == USB_REQ_BULK_ONLY_RESET) {
        assert(setup->request_type == (USB_TYPE_CLASS | USB_RECIP_INTERFACE));
        assert(setup->index == 0);
        storage_record('R');
        stage = 1;
    } else {
        assert(setup->request == USB_REQ_CLEAR_FEATURE);
        assert(setup->request_type == (USB_TYPE_STANDARD | USB_RECIP_ENDPOINT));
        assert(setup->index == 0x81 || setup->index == 2);
        storage_record(setup->index == 0x81 ? 'i' : 'o');
        stage = setup->index == 0x81 ? 2 : 3;
    }
    if (stage == storage_control_fail) return -ETIMEDOUT;
    if (stage == 1) storage_phase = 0;
    return 0;
}

static int storage_configure(usb_hcd_t *h, usb_device_t *u, uint8_t addr,
                             uint8_t type, uint16_t mps, uint8_t interval) {
    (void)h;
    assert(u->ifaces[0].device->drv_priv && storage_interfaces);
    assert(type == USB_XFER_BULK && mps == 512 && !interval);
    assert(!g_storage_lock.held);
    storage_configures++;
    usb_storage_dev_t *st = u->ifaces[0].device->drv_priv;
    if (st->recovery_pending) {
        assert(st->lock.held);
        storage_record(addr == 2 ? 'A' : 'B');
    }
    storage_owned |= addr == 2 ? 1 : 2;
    return (int)storage_configures == storage_configure_fail ? -ETIMEDOUT : 0;
}

static int storage_release_endpoint(usb_hcd_t *h, usb_device_t *u, uint8_t addr) {
    (void)h;
    usb_storage_dev_t *st = u->ifaces[0].device->drv_priv;
    assert(st && !g_storage_lock.held);
    if (st->recovery_pending && !st->closing) {
        assert(st->lock.held);
        storage_record(addr == 0x81 ? 'I' : 'O');
    } else assert(st->closing && !st->active_calls);
    storage_releases++;
    if ((int)storage_releases == storage_release_fail) return -ETIMEDOUT;
    storage_owned &= ~(addr == 2 ? 1U : 2U);
    return 0;
}

static int storage_bulk(usb_hcd_t *h, usb_urb_t *urb) {
    (void)h;
    assert(!g_storage_lock.held);
    usb_storage_dev_t *st = urb->dev->ifaces[0].device->drv_priv;
    assert(st && st->lock.held);
    if (!storage_phase) storage_cbws++;
    if ((int)storage_phase + 1 == storage_bulk_fail_stage) {
        storage_bulk_fail_stage = 0;
        return -EIO;
    }
    if (!storage_phase) {
        assert(urb->ep->addr == 2 && urb->len == USB_MSC_CBW_SIZE);
        memcpy(&storage_cbw, urb->buf, sizeof(storage_cbw));
        assert(storage_cbw.signature == USB_MSC_CBW_SIGNATURE);
        storage_transferred = 0;
        storage_phase = storage_cbw.transfer_length ? 1 : 2;
        if (storage_stop_in_io && storage_cbw.command[0] == SCSI_CMD_READ10) {
            storage_stop_in_io = 0;
            unsigned releases = storage_releases;
            assert(st->active_calls == 1);
            driver_lifecycle_begin();
            assert(device_unbind(st->owner) == -EBUSY);
            driver_lifecycle_end();
            assert(st->owner && st->endpoints_live && st->active_calls == 1);
            assert(storage_releases == releases && !usb_storage_get_dev(0));
        }
    } else if (storage_phase == 1) {
        assert(urb->len <= USB_MSC_IO_CHUNK);
        if (storage_cbw.command[0] == SCSI_CMD_READ_CAPACITY10) {
            assert(urb->len == 8);
            msc_put_be32(urb->buf, 127);
            msc_put_be32((uint8_t *)urb->buf + 4, 512);
        } else {
            assert(storage_cbw.command[0] == SCSI_CMD_READ10 ||
                   storage_cbw.command[0] == SCSI_CMD_REQUEST_SENSE);
            memset(urb->buf, 0xa5, urb->len);
        }
        storage_transferred += urb->len;
        if (storage_transferred == storage_cbw.transfer_length) storage_phase = 2;
    } else {
        assert(urb->ep->addr == 0x81 && urb->len == USB_MSC_CSW_SIZE);
        usb_msc_csw_t csw = { .signature = USB_MSC_CSW_SIGNATURE,
                              .tag = storage_cbw.tag };
        switch (storage_csw_fault) {
        case 1: csw.tag++; break;
        case 2: csw.signature++; csw.status = 1; break;
        case 3: csw.status = 2; break;
        case 4: csw.status = 3; break;
        case 5: csw.residue = storage_cbw.transfer_length + 1; break;
        case 6: csw.status = 1; break;
        case 7: csw.residue = 1; break;
        }
        storage_csw_fault = 0;
        memcpy(urb->buf, &csw, sizeof(csw));
        storage_phase = 0;
    }
    return 0;
}

static int storage_abort(usb_hcd_t *h, uint8_t slot) {
    (void)h;
    assert(slot == 1);
    storage_owned = 0;
    return 0;
}

static device_t *storage_publish(usb_hcd_t *h) {
    usb_device_t *u = usb_create(h, 1);
    u->slot = 1;
    usb_interface_t *iface = u->ifaces;
    iface->interface_class = USB_CLASS_MASS_STORAGE;
    iface->interface_subclass = USB_SUBCLASS_SCSI;
    iface->interface_protocol = USB_PROTOCOL_BULK_ONLY;
    kfree(iface->eps);
    iface->eps = kcalloc(2, sizeof(*iface->eps));
    iface->ep_count = 2;
    iface->eps[0] = (usb_endpoint_t){ .addr = 2, .attrs = USB_XFER_BULK,
                                     .max_packet = 512 };
    iface->eps[1] = (usb_endpoint_t){ .addr = 0x81, .attrs = USB_XFER_BULK,
                                     .max_packet = 512 };
    h->port_devices[0] = u;
    assert(!usb_publish_interfaces(u));
    return iface->device;
}

static void storage_case(const char *name) {
    static const usb_hcd_ops_t ops = { .control = storage_control,
        .configure_endpoint = storage_configure, .release_endpoint = storage_release_endpoint,
        .submit_bulk = storage_bulk, .abort_slot = storage_abort };
    device_t parent = { .name = "hcd", .references = 1 };
    usb_device_t *ports[1] = {0};
    uint8_t state[1] = {0};
    usb_hcd_t h = { .hcd_dev = &parent, .ops = &ops, .max_ports = 1,
                    .port_devices = ports, .port_state = state };
    g_hcds[0] = &h;
    g_hcd_count = 1;
    g_drivers[0] = &usb_storage_driver;
    g_driver_count = 1;
    if (!strcmp(name, "partial")) {
        storage_configure_fail = 2;
        storage_release_fail = 2;
        device_t *dev = storage_publish(&h);
        usb_storage_dev_t *st = dev->drv_priv;
        assert(st == &g_storage[0] && st->owner == dev && st->closing);
        assert(st->endpoints_live && storage_owned == 2 && !usb_storage_ready(0));
        /* A failed cleanup owner cannot be overwritten by the next claim. */
        device_t peer = {0};
        assert(msc_claim(&peer) == &g_storage[1]);
        assert(!usb_storage_remove(&peer));
        storage_release_fail = 0;
        driver_lifecycle_begin();
        assert(!device_unbind(dev));
        driver_lifecycle_end();
        assert(!dev->drv_priv && !st->owner && !storage_owned);
    } else if (!strcmp(name, "active")) {
        device_t *dev = storage_publish(&h);
        block_dev_t *block = usb_storage_get_dev(0);
        assert(block && usb_storage_get_dev(0) == block);
        usb_storage_dev_t *st = block->priv;
        assert(usb_storage_ready(0) && st->consumer_refs == 2);
        uint8_t data[8192] = {0};
        assert(block->read_sector(block, 128, data, 1) == -EIO && !st->active_calls);
        storage_stop_in_io = 1;
        assert(!block->read_sector(block, 0, data, 16));
        for (unsigned i = 0; i < sizeof(data); i++) assert(data[i] == 0xa5);
        assert(!st->active_calls && st->closing && st->endpoints_live);
        storage_release_fail = storage_releases + 2;
        driver_lifecycle_begin();
        assert(device_unbind(dev) == -ETIMEDOUT);
        driver_lifecycle_end();
        assert(dev->drv_priv == st && st->owner && st->consumer_refs == 2);
        storage_release_fail = 0;
        driver_lifecycle_begin();
        assert(!device_unbind(dev));
        driver_lifecycle_end();
        assert(!st->owner && !st->iface && !storage_owned);
        assert(block->read_sector(block, 0, data, 1) == -ENODEV);
        device_t peer = {0};
        assert(msc_claim(&peer) == &g_storage[1]);
        assert(!usb_storage_remove(&peer));
        block->release(block);
        assert(msc_claim(&peer) == &g_storage[1]);
        assert(!usb_storage_remove(&peer));
        block->release(block);
        assert(msc_claim(&peer) == &g_storage[0]);
        assert(!usb_storage_remove(&peer));
    } else if (!strcmp(name, "recovery")) {
        /* An initial probe error recovers while lifecycle is already owned. */
        storage_bulk_fail_stage = 1;
        device_t *dev = storage_publish(&h);
        block_dev_t *block = usb_storage_get_dev(0);
        assert(block && !strcmp(storage_trace, "IORioAB"));
        usb_storage_dev_t *st = block->priv;
        uint8_t data[512] = {0};
        for (int fault = 1; fault <= 7; fault++) {
            storage_trace_len = 0; storage_trace[0] = 0;
            storage_csw_fault = fault;
            unsigned cbws = storage_cbws;
            assert(block->read_sector(block, 0, data, 1) == -EIO);
            assert(storage_cbws == cbws + 1 && !st->recovery_pending);
            assert(!strcmp(storage_trace, fault <= 5 ? "IORioAB" : ""));
        }
        /* A failed recovery remains pending and blocks new CBWs. */
        storage_trace_len = 0; storage_trace[0] = 0;
        storage_bulk_fail_stage = 1;
        storage_control_fail = 2;
        assert(block->read_sector(block, 0, data, 1) == -EIO);
        assert(st->recovery_pending && !strcmp(storage_trace, "IORi"));
        unsigned cbws = storage_cbws;
        storage_trace_len = 0; storage_trace[0] = 0;
        assert(block->read_sector(block, 0, data, 1) == -ETIMEDOUT);
        assert(st->recovery_pending && storage_cbws == cbws);
        storage_trace_len = 0; storage_trace[0] = 0;
        storage_control_fail = 0;
        assert(!block->read_sector(block, 0, data, 1));
        assert(!st->recovery_pending && !strcmp(storage_trace, "IORioAB"));
        /* Partial host rebuild likewise cannot admit a new command. */
        storage_trace_len = 0; storage_trace[0] = 0;
        storage_bulk_fail_stage = 2;
        storage_configure_fail = storage_configures + 1;
        assert(block->read_sector(block, 0, data, 1) == -EIO);
        assert(st->recovery_pending && storage_owned == 1);
        storage_trace_len = 0; storage_trace[0] = 0;
        storage_configure_fail = 0;
        assert(!block->read_sector(block, 0, data, 1) && !st->recovery_pending);
        /* Runtime recovery never blocks waiting on lifecycle coordination. */
        storage_trace_len = 0; storage_trace[0] = 0;
        storage_bulk_fail_stage = 3;
        driver_lifecycle_begin();
        assert(block->read_sector(block, 0, data, 1) == -EIO);
        assert(st->recovery_pending && !storage_trace_len);
        driver_lifecycle_end();
        assert(!block->read_sector(block, 0, data, 1) && !st->recovery_pending);
        block->release(block);
        assert(dev->drv_priv == st && !st->active_calls);
    } else {
        assert(!strcmp(name, "reuse"));
        for (unsigned i = 0; i < USB_STORAGE_MAX_DEVS + 2; i++) {
            device_t *dev = storage_publish(&h);
            assert(dev->drv_priv == &g_storage[0] && usb_storage_ready(0));
            assert(!usb_disconnect_port(&h, 1));
            assert(!g_storage[0].owner && !ports[0]);
        }
    }
    if (ports[0]) assert(!usb_disconnect_port(&h, 1));
    assert(parent.references == 1 && !storage_owned);
    g_hcd_count = 0;
}
