/* Synaptics CE78 feature reports; private to i2c-hid.c.
 * Matched by descriptor VID/PID and report layout, never by host board name. */
#ifndef A20_I2C_HID_SYNAPTICS_H
#define A20_I2C_HID_SYNAPTICS_H
static int ihid_synaptics_feature(i2c_hid_device_t *h, uint8_t id, uint8_t value)
{
    const uint8_t feature[] = {
        0x30U | id, I2C_HID_CMD_SET_REPORT,
        (uint8_t)h->hd.data_register, (uint8_t)(h->hd.data_register >> 8),
        4, 0, id, value,
    };
    if (!h->hd.data_register)
        return -EINVAL;
    return ihid_write_reg(h, h->hd.command_register, feature, sizeof(feature));
}

static int ihid_synaptics_set_touch_mode(i2c_hid_device_t *h)
{
    /* CE78's actual descriptor has InputMode (4), LatencyMode (13), and
     * SurfaceSwitch/ButtonSwitch (6). Mode alone does not enable reporting.
     * Match the normal/true/true initialization of Linux mt_set_modes(). */
    int rc = ihid_synaptics_feature(h, SYNA_CE78_MODE_REPORT, 3);
    if (!rc)
        rc = ihid_synaptics_feature(h, 13, 0);
    if (!rc)
        rc = ihid_synaptics_feature(h, 6, 3);
    return rc;
}

static int ihid_synaptics_check_features(i2c_hid_device_t *h)
{
    const uint8_t ids[] = {4, 13, 6}, values[] = {3, 0, 3}, masks[] = {255, 1, 3};
    for (unsigned i = 0; i < ARRAY_SIZE(ids); i++) {
        uint8_t tx[] = {(uint8_t)h->hd.command_register,
                        (uint8_t)(h->hd.command_register >> 8),
                        0x30U | ids[i], I2C_HID_CMD_GET_REPORT,
                        (uint8_t)h->hd.data_register,
                        (uint8_t)(h->hd.data_register >> 8)};
        uint8_t rx[4] = {0};
        i2c_msg_t messages[] = {
            {.addr = h->addr, .flags = I2C_M_WRITE, .len = sizeof(tx), .buf = tx},
            {.addr = h->addr, .flags = I2C_M_READ, .len = sizeof(rx), .buf = rx},
        };
        int rc = ihid_xfer(h, messages, 2);
        if (!rc && (le16(rx) != 4 || rx[2] != ids[i] ||
                    (rx[3] & masks[i]) != values[i]))
            rc = -EIO;
        if (h->dev)
            kinfo("[I2C-HID] feature readback id=%u value=%02x len=%u result=%d\n",
                  ids[i], rx[3], le16(rx), rc);
        if (rc)
            return rc;
    }
    return 0;
}

#endif
