/*
 * A20OS HID-over-I2C touchpad driver.
 *
 * Scope and provenance
 * --------------------
 * The wired target machine's touchpad is a Synaptics I2C-HID device
 * (Linux: SYNA2BA6:00 06CB:CE78) behind an AMD DesignWare I2C controller.
 * This driver implements the Microsoft HID-over-I2C protocol as Linux's
 * drivers/hid/i2c-hid/i2c-hid-core.c does for the parts that matter here:
 *
 *   - read the 30-byte HID descriptor from the ACPI _DSM register;
 *   - read the report descriptor from the register it names;
 *   - parse enough of the report descriptor to locate the absolute X/Y, tip
 *     switch, button-1 and (when present) contact-id fields of the touch
 *     report;
 *   - poll the firmware's attention GPIO from a kernel thread and turn
 *     input reports into evdev events for desktop gesture recognition.
 */

#include "drivers/core/driver_class.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "drivers/core/workqueue.h"
#include "drivers/bus/platform_bus.h"
#include "drivers/i2c/i2c.h"
#include "drivers/gpio/firmware_input.h"
#include "drivers/input/evdev.h"
#include "drivers/input/virtio_input.h"
#include "core/bootargs.h"
#include "core/errno.h"
#include "core/klog.h"
#include "core/lock.h"
#include "core/string.h"
#include "core/sync.h"
#include "core/timer.h"
#include "firmware/node.h"
#include "mm/mm.h"
#include "proc/proc.h"

extern void input_mux_wake(void);

/* ---- HID-over-I2C protocol ---------------------------------------------- */
#define I2C_HID_DESC_REG      0x0001U
#define I2C_HID_REPORT_DESC   0x0002U
#define I2C_HID_INPUT_REG     0x0003U
#define I2C_HID_OUTPUT_REG    0x0004U
#define I2C_HID_CMD_REG       0x0005U
#define I2C_HID_DATA_REG      0x0006U

#define I2C_HID_CMD_RESET       0x01U
#define I2C_HID_CMD_GET_REPORT  0x02U
#define I2C_HID_CMD_SET_REPORT  0x03U
#define I2C_HID_CMD_SET_IDLE    0x05U
#define I2C_HID_CMD_SET_PROTOCOL 0x07U
#define I2C_HID_CMD_SET_POWER   0x08U

#define I2C_HID_POWER_ON   0x00U
#define I2C_HID_POWER_SLEEP 0x01U

#define I2C_HID_MAX_REPORT_DESC 4096U
#define I2C_HID_MAX_INPUT       256U
#define I2C_HID_EVENT_RING      256U
#define I2C_HID_FRAME_EVENTS    96U
#define I2C_HID_MAX_FIELDS      48U
#define I2C_HID_MAX_REPORT_IDS  16U

/* BUS_I2C, Linux input_id.bustype. */
#define BUS_I2C 0x18U
#define BTN_LEFT 0x110U /* BTN_MOUSE_FIRST, Linux event code */
#define BTN_TOOL_FINGER 0x145U
/* Multi-touch axes (Linux event codes); core/input.h only carries the
 * single-pointer ABS_X/Y pair. */
#define ABS_MT_SLOT        0x2fU
#define ABS_MT_POSITION_X  0x35U
#define ABS_MT_POSITION_Y  0x36U
#define ABS_MT_TRACKING_ID 0x39U
#define I2C_HID_MAX_CONTACTS 16U
#define SYNA_VENDOR_ID 0x06cbU
#define SYNA_CE78_PRODUCT_ID 0xce78U
#define SYNA_CE78_TOUCH_REPORT 3U
#define SYNA_CE78_MODE_REPORT 4U

typedef struct i2c_hid_descriptor {
    uint16_t length;
    uint16_t bcd_version;
    uint16_t report_desc_length;
    uint16_t report_desc_register;
    uint16_t input_register;
    uint16_t max_input_length;
    uint16_t output_register;
    uint16_t max_output_length;
    uint16_t command_register;
    uint16_t data_register;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t version_id;
} i2c_hid_descriptor_t;

typedef struct i2c_hid_field {
    uint16_t bit_offset;
    int32_t logical_min;
    int32_t logical_max;
    uint8_t bit_size;
    uint8_t report_id;
    uint8_t relative;
    uint8_t present;
    /* Number of consecutive elements this report item declared.  A precision
     * touchpad describes one item per usage with Report Count = contacts, so
     * contact i lives at bit_offset + i * bit_size. */
    uint8_t count;
    uint8_t stride_bits;
    uint16_t page;
    uint16_t usage;
} i2c_hid_field_t;

typedef struct i2c_hid_device {
    device_t *dev;
    i2c_adapter_t *adapter;
    uint16_t addr;
    i2c_hid_descriptor_t hd;

    uint8_t *report_desc;
    uint8_t *input_buf;
    uint16_t input_buf_len;
    gpio_firmware_input_t attention;
    uint8_t attention_seen, invalid_input_logged;

    i2c_hid_field_t fields[I2C_HID_MAX_FIELDS];
    unsigned field_count;
    uint8_t report_ids[I2C_HID_MAX_REPORT_IDS];
    unsigned report_id_count;
    int uses_report_ids;
    uint16_t max_report_bytes;

    /* Field index of interest, -1 when the descriptor has no such field. */
    int f_x, f_y, f_tip, f_button, f_contact_id;
    uint8_t active_report_id;

    struct input_event events[I2C_HID_EVENT_RING];
    unsigned head, tail;
    spinlock_t event_lock;
    /* The I/O mutex owns construction; readers see only complete frames. */
    struct input_event frame[I2C_HID_FRAME_EVENTS];
    unsigned frame_count;

    mutex_t io_lock;
    uint32_t poll_ms;
    volatile int present;
    volatile int running;

    /* Last absolute position, for change detection. */
    int32_t last_x, last_y;
    uint16_t last_contacts;
    uint8_t last_tip, last_button;
    uint8_t first_report_logged, first_event_logged, poll_error_logged;
    uint8_t axis_resolution;
    uint64_t reads, valid_reports, read_errors, gpio_skips;
    uint64_t touch_frames, movement_frames;
    uint64_t queue_overflows, recoveries, recover_at;
    uint8_t recovery_pending;
    struct driver_workqueue *queue;
    struct driver_work *work;
    uint64_t report_at;
} i2c_hid_device_t;

static int ihid_scheduler_ready;
static void ihid_poll_work(void *context);

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static void ihid_setup_attention(i2c_hid_device_t *h, const fw_node_t *node)
{
    if (!gpio_firmware_input_get(node, "acpi,gpio-attention", &h->attention))
        kinfo("[I2C-HID] firmware attention input pin=%08x\n",
              readl(h->attention.reg));
}

static int ihid_attention_pending(const i2c_hid_device_t *h)
{
    return gpio_firmware_input_pending(&h->attention);
}

/* ---- register access ----------------------------------------------------- */
static int ihid_xfer(i2c_hid_device_t *h, i2c_msg_t *msgs, int n)
{
    if (!h->adapter || !h->adapter->transfer)
        return -ENODEV;
    return h->adapter->transfer(h->adapter, msgs, n);
}

static int ihid_read_reg(i2c_hid_device_t *h, uint16_t reg, void *buf,
                         uint16_t len)
{
    uint8_t addr[2] = {(uint8_t)(reg & 0xffU), (uint8_t)(reg >> 8)};
    i2c_msg_t msgs[2] = {
        {.addr = h->addr, .flags = I2C_M_WRITE, .len = sizeof(addr), .buf = addr},
        {.addr = h->addr, .flags = I2C_M_READ, .len = len, .buf = buf},
    };
    return ihid_xfer(h, msgs, 2);
}

static int ihid_write_reg(i2c_hid_device_t *h, uint16_t reg, const void *buf,
                          uint16_t len)
{
    uint8_t tmp[I2C_HID_MAX_INPUT + 2];
    if ((uint32_t)len + 2U > sizeof(tmp))
        return -EINVAL;
    tmp[0] = (uint8_t)(reg & 0xffU);
    tmp[1] = (uint8_t)(reg >> 8);
    if (len)
        memcpy(tmp + 2, buf, len);
    i2c_msg_t msg = {.addr = h->addr, .flags = I2C_M_WRITE,
                     .len = (uint16_t)(len + 2U), .buf = tmp};
    return ihid_xfer(h, &msg, 1);
}

/*
 * Command framing (i2c_hid_encode_command() in Linux): the command-register
 * payload is
 *
 *     byte 0 = (report_type << 4) | report_id     (report_id < 0x0F)
 *     byte 1 = opcode
 *     byte 2 = report_id                          (only when report_id >= 0x0F)
 *
 * An earlier version of this driver wrote the opcode first, which no device
 * accepts.  SET_POWER passes the power state as the "report id" argument, so
 * its payload is [state, 0x08].
 */
static int ihid_command(i2c_hid_device_t *h, uint8_t report_type,
                        uint8_t report_id, uint8_t opcode)
{
    uint8_t cmd[4];
    uint16_t n = 0;
    if (report_id < 0x0fU) {
        cmd[n++] = (uint8_t)((report_type << 4) | report_id);
        cmd[n++] = opcode;
    } else {
        cmd[n++] = (uint8_t)((report_type << 4) | 0x0fU);
        cmd[n++] = opcode;
        cmd[n++] = report_id;
    }
    return ihid_write_reg(h, h->hd.command_register, cmd, n);
}

static int ihid_set_power(i2c_hid_device_t *h, uint8_t state)
{
    int rc = ihid_command(h, 0, state, I2C_HID_CMD_SET_POWER);
    if (rc < 0 && state == I2C_HID_POWER_ON) {
        /* Some STM-based devices need a clock edge to wake and NAK the first
         * request (i2c_hid_set_power()); retry once after 400 us. */
        udelay(400);
        rc = ihid_command(h, 0, state, I2C_HID_CMD_SET_POWER);
    }
    return rc;
}

#include "i2c-hid-synaptics.h"

/* The input register is read with a plain transfer: the device treats a bare
 * read (no register-address phase) as "next input report", which is what
 * keeps the attention path cheap (Linux i2c_hid_get_input() uses
 * i2c_master_recv()). */
static int ihid_read_input_raw(i2c_hid_device_t *h, uint8_t *buf, uint16_t len)
{
    i2c_msg_t msg = {.addr = h->addr, .flags = I2C_M_READ, .len = len,
                     .buf = buf};
    return ihid_xfer(h, &msg, 1);
}

/* Reset completion is a zero-length input report: the first two bytes of the
 * input register read back as zero (i2c_hid_get_input()). */
static int ihid_reset(i2c_hid_device_t *h)
{
    int rc = ihid_command(h, 0, 0, I2C_HID_CMD_RESET);
    if (rc < 0)
        return rc;
    uint64_t deadline = clock_get_ticks() + MS_TO_TICKS(1000);
    for (unsigned i = 0; i < 1000 && clock_get_ticks() < deadline; i++) {
        if (ihid_attention_pending(h) &&
            ihid_read_input_raw(h, h->input_buf, h->input_buf_len) == 0 &&
            le16(h->input_buf) == 0)
            return 0;
        mdelay(1);
    }
    return -ETIMEDOUT;
}

static int ihid_fetch_descriptor(i2c_hid_device_t *h, uint16_t reg)
{
    uint8_t buf[30];
    int rc = ihid_read_reg(h, reg, buf, sizeof(buf));
    if (rc < 0)
        return rc;
    i2c_hid_descriptor_t *d = &h->hd;
    d->length = le16(buf + 0);
    d->bcd_version = le16(buf + 2);
    d->report_desc_length = le16(buf + 4);
    d->report_desc_register = le16(buf + 6);
    d->input_register = le16(buf + 8);
    d->max_input_length = le16(buf + 10);
    d->output_register = le16(buf + 12);
    d->max_output_length = le16(buf + 14);
    d->command_register = le16(buf + 16);
    d->data_register = le16(buf + 18);
    d->vendor_id = le16(buf + 20);
    d->product_id = le16(buf + 22);
    d->version_id = le16(buf + 24);

    if (d->length < 30 || d->bcd_version != 0x0100U ||
        d->max_input_length > I2C_HID_MAX_INPUT ||
        d->report_desc_length == 0 || d->command_register == 0 ||
        d->input_register == 0) {
        kwarn("[I2C-HID] invalid descriptor: length=%u version=%04x report=%u input=%u command=%04x\n",
              d->length, d->bcd_version, d->report_desc_length,
              d->max_input_length, d->command_register);
        return -ENODEV;
    }
    /* wMaxInputLength is wrong on some devices (Linux's
     * i2c_hid_find_max_report() exists for that); 0 here means "derive it
     * from the report descriptor after parsing". */
    return 0;
}

/* ---- HID report descriptor parser --------------------------------------- */
#define HID_PAGE_GENERIC   0x01U
#define HID_PAGE_BUTTON    0x09U
#define HID_PAGE_DIGITIZER 0x0DU
#define HID_USAGE_X        0x30U
#define HID_USAGE_Y        0x31U
#define HID_USAGE_TIP      0x42U
#define HID_USAGE_CONTACT  0x51U
#define HID_USAGE_BUTTON1  0x01U

typedef struct hid_parser {
    uint32_t usage_page;
    int32_t logical_min;
    int32_t logical_max;
    uint32_t report_size;
    uint32_t report_count;
    uint32_t report_id;
    struct { uint32_t page, usage; } local[32];
    unsigned local_count;
    uint32_t usage_min, usage_max;
    int usage_range;
    /* Bit cursor per report id; index is the id itself (0..15). */
    uint32_t bitpos[I2C_HID_MAX_REPORT_IDS];
} hid_parser_t;

static i2c_hid_field_t *hid_add_field(i2c_hid_device_t *h, uint16_t page,
                                      uint16_t usage, uint32_t bit_offset,
                                      uint8_t bit_size, uint8_t count,
                                      int32_t lmin, int32_t lmax,
                                      uint8_t report_id, uint8_t relative)
{
    if (h->field_count >= I2C_HID_MAX_FIELDS)
        return NULL;
    i2c_hid_field_t *f = &h->fields[h->field_count++];
    *f = (i2c_hid_field_t){
        .bit_offset = (uint16_t)bit_offset,
        .logical_min = lmin,
        .logical_max = lmax,
        .bit_size = bit_size,
        .count = count,
        .report_id = report_id,
        .relative = relative,
        .present = 1,
        .page = page,
        .usage = usage,
    };
    return f;
}

static int hid_wanted(uint16_t page, uint16_t usage)
{
    if (page == HID_PAGE_GENERIC && (usage == HID_USAGE_X || usage == HID_USAGE_Y))
        return 1;
    if (page == HID_PAGE_DIGITIZER &&
        (usage == HID_USAGE_TIP || usage == HID_USAGE_CONTACT))
        return 1;
    if (page == HID_PAGE_BUTTON && usage == HID_USAGE_BUTTON1)
        return 1;
    return 0;
}

/* One global-state snapshot for Push/Pop. */
typedef struct hid_globals {
    uint32_t usage_page;
    int32_t logical_min, logical_max;
    uint32_t report_size, report_count, report_id;
} hid_globals_t;

static void hid_parse_report_descriptor(i2c_hid_device_t *h)
{
    hid_parser_t p;
    memset(&p, 0, sizeof(p));
    hid_globals_t stack[4];
    unsigned depth = 0;
    const uint8_t *d = h->report_desc;
    uint32_t n = h->hd.report_desc_length;
    uint32_t i = 0;

    while (i < n) {
        uint8_t prefix = d[i];
        if (prefix == 0xFEU) { /* long item: skip */
            if (i + 3U > n)
                break;
            uint32_t len = d[i + 1];
            if (i + 3U + len > n)
                break;
            i += 3U + len;
            continue;
        }
        unsigned size_code = prefix & 3U;
        unsigned len = size_code == 3U ? 4U : size_code;
        unsigned type = (prefix >> 2) & 3U;
        unsigned tag = prefix >> 4;
        if (i + 1U + len > n)
            break;
        uint32_t data = 0;
        for (unsigned b = 0; b < len; b++)
            data |= (uint32_t)d[i + 1U + b] << (8U * b);
        i += 1U + len;

        if (type == 1U) { /* Global */
            switch (tag) {
                case 0: p.usage_page = data & 0xffffU; break;
                case 1: {
                    int32_t v = (int32_t)data;
                    if (len == 1 && (data & 0x80U)) v = (int8_t)data;
                    else if (len == 2 && (data & 0x8000U)) v = (int16_t)data;
                    p.logical_min = v;
                    break;
                }
                case 2: {
                    int32_t v = (int32_t)data;
                    if (len == 1 && (data & 0x80U)) v = (int8_t)data;
                    else if (len == 2 && (data & 0x8000U)) v = (int16_t)data;
                    p.logical_max = v;
                    break;
                }
                case 7: p.report_size = data; break;
                case 8: p.report_id = data & 0xffU; break;
                case 9: p.report_count = data; break;
                case 10: /* Push */
                    if (depth < 4) {
                        stack[depth++] = (hid_globals_t){
                            p.usage_page, p.logical_min, p.logical_max,
                            p.report_size, p.report_count, p.report_id};
                    }
                    break;
                case 11: /* Pop */
                    if (depth > 0) {
                        hid_globals_t *g = &stack[--depth];
                        p.usage_page = g->usage_page;
                        p.logical_min = g->logical_min;
                        p.logical_max = g->logical_max;
                        p.report_size = g->report_size;
                        p.report_count = g->report_count;
                        p.report_id = g->report_id;
                    }
                    break;
                default: break;
            }
        } else if (type == 2U) { /* Local */
            if (tag == 0) { /* Usage */
                uint32_t page = p.usage_page;
                uint32_t usage = data;
                if (len == 4) {
                    page = data >> 16;
                    usage = data & 0xffffU;
                }
                if (p.local_count < 32)
                    p.local[p.local_count++] = (typeof(p.local[0])){page, usage};
                p.usage_range = 0;
            } else if (tag == 1) { /* Usage Minimum */
                p.usage_min = data;
                p.usage_range = 1;
            } else if (tag == 2) { /* Usage Maximum */
                p.usage_max = data;
            } else if (tag == 10) { /* Delimiter */
                /* A delimiter groups an alternative usage set; the first
                 * alternative is what a touchpad exposes, so ignore it. */
            }
        } else if (type == 0U) { /* Main */
            uint8_t rid = (uint8_t)p.report_id;
            uint32_t *cursor = &p.bitpos[rid < I2C_HID_MAX_REPORT_IDS ? rid : 0];
            if (tag == 8U) { /* Input */
                int variable = (data & 0x02U) != 0;
                if (rid && h->report_id_count < I2C_HID_MAX_REPORT_IDS) {
                    int seen = 0;
                    for (unsigned k = 0; k < h->report_id_count; k++)
                        if (h->report_ids[k] == rid) seen = 1;
                    if (!seen) {
                        h->report_ids[h->report_id_count++] = rid;
                        h->uses_report_ids = 1;
                    }
                }
                for (uint32_t c = 0; c < p.report_count; c++) {
                    if (variable && !(data & 0x01U)) {
                        uint32_t page = p.usage_page;
                        uint32_t usage = 0;
                        if (p.local_count) {
                            if (c < p.local_count) {
                                page = p.local[c].page;
                                usage = p.local[c].usage;
                            }
                        } else if (p.usage_range) {
                            usage = p.usage_min + c;
                        }
                        if (hid_wanted((uint16_t)page, (uint16_t)usage))
                            (void)hid_add_field(h, (uint16_t)page,
                                                (uint16_t)usage, *cursor,
                                                (uint8_t)p.report_size,
                                                /* One usage repeated N times means
                                                 * N consecutive elements of the
                                                 * same usage (how a touchpad lays
                                                 * out contacts).  Several usages in
                                                 * one item means one element each. */
                                                (uint8_t)((p.local_count > 1 || p.report_count == 0
                                                               ? 1U
                                                               : (p.report_count > I2C_HID_MAX_CONTACTS
                                                                      ? I2C_HID_MAX_CONTACTS
                                                                      : p.report_count))),
                                                p.logical_min, p.logical_max,
                                                rid, (uint8_t)((data >> 2) & 1U));
                    }
                    *cursor += p.report_size;
                }
                if (*cursor > (uint32_t)h->max_report_bytes * 8U)
                    h->max_report_bytes = (uint16_t)((*cursor + 7U) / 8U);
                p.local_count = 0;
                p.usage_range = 0;
            } else if (tag == 9U || tag == 11U) { /* Output / Feature */
                *cursor += p.report_size * p.report_count;
                p.local_count = 0;
                p.usage_range = 0;
            } else if (tag == 10U || tag == 12U) { /* Collection / End */
                p.local_count = 0;
                p.usage_range = 0;
            } else {
                p.local_count = 0;
                p.usage_range = 0;
            }
        }
    }
}

static int ihid_find_field(i2c_hid_device_t *h, uint16_t page, uint16_t usage,
                           uint8_t report_id)
{
    for (unsigned i = 0; i < h->field_count; i++) {
        i2c_hid_field_t *f = &h->fields[i];
        if (f->page == page && f->usage == usage &&
            f->report_id == report_id)
            return (int)i;
    }
    return -1;
}

/* Pick the report that carries the pointer.  Prefer one with a tip switch
 * (a touch report) over a bare X/Y report, and never pick a report whose X/Y
 * live in different ids. */
static int ihid_choose_report(i2c_hid_device_t *h)
{
    int best = -1;
    for (unsigned r = 0; r < h->report_id_count; r++) {
        uint8_t rid = h->report_ids[r];
        int x = ihid_find_field(h, HID_PAGE_GENERIC, HID_USAGE_X, rid);
        int y = ihid_find_field(h, HID_PAGE_GENERIC, HID_USAGE_Y, rid);
        if (x < 0 || y < 0)
            continue;
        int tip = ihid_find_field(h, HID_PAGE_DIGITIZER, HID_USAGE_TIP, rid);
        h->f_x = x;
        h->f_y = y;
        h->f_tip = tip;
        h->f_button = ihid_find_field(h, HID_PAGE_BUTTON, HID_USAGE_BUTTON1, rid);
        h->f_contact_id = ihid_find_field(h, HID_PAGE_DIGITIZER,
                                          HID_USAGE_CONTACT, rid);
        h->active_report_id = rid;
        best = (int)rid;
        if (tip >= 0)
            break;
    }
    if (best < 0) {
        /* No report ids at all: HID allows a single report without a prefix. */
        int x = ihid_find_field(h, HID_PAGE_GENERIC, HID_USAGE_X, 0);
        int y = ihid_find_field(h, HID_PAGE_GENERIC, HID_USAGE_Y, 0);
        if (x >= 0 && y >= 0) {
            h->f_x = x;
            h->f_y = y;
            h->f_tip = ihid_find_field(h, HID_PAGE_DIGITIZER, HID_USAGE_TIP, 0);
            h->f_button = ihid_find_field(h, HID_PAGE_BUTTON, HID_USAGE_BUTTON1, 0);
            h->f_contact_id = ihid_find_field(h, HID_PAGE_DIGITIZER,
                                              HID_USAGE_CONTACT, 0);
            h->active_report_id = 0;
            best = 0;
        }
    }
    return best;
}

static int ihid_configure_synaptics(i2c_hid_device_t *h)
{
    if (h->hd.vendor_id != SYNA_VENDOR_ID ||
        h->hd.product_id != SYNA_CE78_PRODUCT_ID ||
        h->active_report_id != SYNA_CE78_TOUCH_REPORT || h->f_tip < 0)
        return 0;

    /* CE78 report 3 has five 40-bit Finger collections. Verify all copies
     * before using the stride; other revisions can keep the first contact. */
    const int indices[] = {h->f_tip, h->f_x, h->f_y};
    for (unsigned i = 0; i < ARRAY_SIZE(indices); i++) {
        const i2c_hid_field_t *first = &h->fields[indices[i]];
        for (unsigned finger = 1; finger < 5; finger++) {
            unsigned j;
            for (j = 0; j < h->field_count; j++) {
                const i2c_hid_field_t *f = &h->fields[j];
                if (f->report_id == first->report_id && f->page == first->page &&
                    f->usage == first->usage && f->bit_size == first->bit_size &&
                    f->bit_offset == first->bit_offset + 40U * finger)
                    break;
            }
            if (j == h->field_count)
                return ihid_synaptics_set_touch_mode(h);
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(indices); i++) {
        h->fields[indices[i]].count = 5;
        h->fields[indices[i]].stride_bits = 40;
    }
    /* Logical 1404 × 864 over physical 116.8 × 72.0 mm in this descriptor. */
    if (h->fields[h->f_x].logical_max == 1404 &&
        h->fields[h->f_y].logical_max == 864)
        h->axis_resolution = 12;
    return ihid_synaptics_set_touch_mode(h);
}

static uint32_t hid_extract(const uint8_t *report, size_t len,
                            const i2c_hid_field_t *f)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < f->bit_size && i < 32U; i++) {
        uint32_t bit = (uint32_t)f->bit_offset + i;
        if (bit / 8U >= len)
            break;
        v |= (uint32_t)((report[bit / 8U] >> (bit % 8U)) & 1U) << i;
    }
    /* Sign-extend a signed field. */
    if (f->logical_min < 0 && f->bit_size > 0 && f->bit_size < 32U) {
        uint32_t sign = 1U << (f->bit_size - 1U);
        if (v & sign)
            v |= ~((1U << f->bit_size) - 1U);
    }
    return v;
}

static int32_t ihid_field_value(i2c_hid_device_t *h, const uint8_t *report,
                                size_t len, int index)
{
    if (!h || index < 0)
        return 0;
    return (int32_t)hid_extract(report, len, &h->fields[index]);
}

/* Element @elem of a multi-element report item, or 0 when the item did not
 * declare that many elements.  Precision-touchpad report descriptors lay the
 * contacts out consecutively per usage, so this is contact @elem. */
static int32_t ihid_field_element(i2c_hid_device_t *h, const uint8_t *report,
                                  size_t len, int index, unsigned elem)
{
    if (!h || index < 0)
        return 0;
    i2c_hid_field_t *f = &h->fields[index];
    if (elem >= f->count)
        return 0;
    i2c_hid_field_t shifted = *f;
    unsigned stride = f->stride_bits ? f->stride_bits : f->bit_size;
    shifted.bit_offset = (uint16_t)(f->bit_offset + elem * stride);
    return (int32_t)hid_extract(report, len, &shifted);
}

/* ---- event queue --------------------------------------------------------- */
static void ihid_emit(i2c_hid_device_t *h, uint16_t type, uint16_t code,
                      int32_t value)
{
    if (h->frame_count < I2C_HID_FRAME_EVENTS)
        h->frame[h->frame_count++] = (struct input_event){
            .type = type, .code = code, .value = value};
}

static int ihid_publish(i2c_hid_device_t *h, int resync)
{
    uint64_t flags = spin_lock_irqsave(&h->event_lock);
    unsigned free = (h->tail + I2C_HID_EVENT_RING - h->head - 1) % I2C_HID_EVENT_RING;
    if (!resync && free < h->frame_count) {
        spin_unlock_irqrestore(&h->event_lock, flags);
        return 0; /* Rebuild a full current-state frame before dropping. */
    }
    if (resync) {
        h->queue_overflows++;
        h->head = h->tail = 0;
        h->events[h->head++] = (struct input_event){.type=EV_SYN, .code=SYN_DROPPED};
    }
    for (unsigned i = 0; i < h->frame_count; i++) {
        h->events[h->head] = h->frame[i];
        h->head = (h->head + 1) % I2C_HID_EVENT_RING;
    }
    h->frame_count = 0;
    spin_unlock_irqrestore(&h->event_lock, flags);
    input_mux_wake();
    return 1;
}

static void ihid_handle_report(i2c_hid_device_t *h, const uint8_t *report,
                               size_t len)
{
    if (!len)
        return;
    uint8_t rid = 0;
    if (h->uses_report_ids) {
        rid = report[0];
        report++;
        len--;
    }
    if (rid != h->active_report_id)
        return;

    int resync = 0;
build_frame:
    h->frame_count = 0;
    int sent = resync;
    int32_t x = ihid_field_value(h, report, len, h->f_x);
    int32_t y = ihid_field_value(h, report, len, h->f_y);
    i2c_hid_field_t *xf = h->f_x >= 0 ? &h->fields[h->f_x] : NULL;

    if (xf && xf->relative) {
        /* Boot-mouse style report: relative deltas and buttons. */
        if (x || y) {
            ihid_emit(h, EV_REL, REL_X, x);
            ihid_emit(h, EV_REL, REL_Y, y);
            sent = 1;
        }
    } else if (xf && xf->count > 1) {
        /* Precision-touchpad report: one slot per contact.  Slots absent from
         * this report get a -1 tracking id so consumers drop the contact. */
        unsigned contacts = xf->count;
        if (contacts > I2C_HID_MAX_CONTACTS)
            contacts = I2C_HID_MAX_CONTACTS;
        int any_tip = 0;
        for (unsigned i = 0; i < contacts; i++) {
            int32_t tipv = h->f_tip >= 0
                               ? ihid_field_element(h, report, len, h->f_tip, i)
                               : (i == 0);
            int active = tipv != 0;
            uint8_t was = (h->last_contacts >> i) & 1U;
            if (active) {
                int32_t slot_x = ihid_field_element(h, report, len, h->f_x, i);
                int32_t slot_y = ihid_field_element(h, report, len, h->f_y, i);
                if (!any_tip) {
                    x = slot_x;
                    y = slot_y;
                    if (resync || !h->last_tip || x != h->last_x || y != h->last_y) {
                        ihid_emit(h, EV_ABS, ABS_X, x);
                        ihid_emit(h, EV_ABS, ABS_Y, y);
                    }
                    any_tip = 1;
                }
                ihid_emit(h, EV_ABS, ABS_MT_SLOT, (int32_t)i);
                if (resync || !was)
                    ihid_emit(h, EV_ABS, ABS_MT_TRACKING_ID, (int32_t)i + 1);
                ihid_emit(h, EV_ABS, ABS_MT_POSITION_X, slot_x);
                ihid_emit(h, EV_ABS, ABS_MT_POSITION_Y, slot_y);
                sent = 1;
            } else if (resync || was) {
                ihid_emit(h, EV_ABS, ABS_MT_SLOT, (int32_t)i);
                ihid_emit(h, EV_ABS, ABS_MT_TRACKING_ID, -1);
                sent = 1;
            }
        }
        h->last_contacts = 0;
        if (any_tip) {
            for (unsigned i = 0; i < contacts; i++)
                if (h->f_tip < 0 || ihid_field_element(h, report, len,
                                                       h->f_tip, i))
                    h->last_contacts |= (uint16_t)(1U << i);
        }
        if (resync || any_tip != h->last_tip) {
            ihid_emit(h, EV_KEY, BTN_TOUCH, any_tip);
            ihid_emit(h, EV_KEY, BTN_TOOL_FINGER, any_tip);
            sent = 1;
        }
        if (any_tip && !resync) {
            h->touch_frames++;
            if (x != h->last_x || y != h->last_y)
                h->movement_frames++;
        }
        h->last_tip = (uint8_t)any_tip;
        h->last_x = x;
        h->last_y = y;
    } else {
        uint8_t tip = h->f_tip >= 0
                          ? (ihid_field_value(h, report, len, h->f_tip) != 0)
                          : 1;
        if (tip) {
            if (resync || !h->last_tip) {
                ihid_emit(h, EV_KEY, BTN_TOOL_FINGER, 1);
                ihid_emit(h, EV_KEY, BTN_TOUCH, 1);
            }
            if (resync || !h->last_tip || x != h->last_x || y != h->last_y) {
                ihid_emit(h, EV_ABS, ABS_X, x);
                ihid_emit(h, EV_ABS, ABS_Y, y);
            }
            sent = 1;
        } else if (resync || h->last_tip) {
            ihid_emit(h, EV_KEY, BTN_TOUCH, 0);
            ihid_emit(h, EV_KEY, BTN_TOOL_FINGER, 0);
            sent = 1;
        }
        h->last_tip = tip;
        h->last_x = x;
        h->last_y = y;
    }

    if (h->f_button >= 0) {
        uint8_t button = ihid_field_value(h, report, len, h->f_button) != 0;
        if (resync || button != h->last_button) {
            ihid_emit(h, EV_KEY, BTN_LEFT, button);
            h->last_button = button;
            sent = 1;
        }
    }
    if (sent) {
        ihid_emit(h, EV_SYN, SYN_REPORT, 0);
        if (!ihid_publish(h, resync)) {
            resync = 1;
            goto build_frame;
        }
        if (h->dev && !h->first_event_logged) {
            h->first_event_logged = 1;
            kinfo("[I2C-HID] first pointer event queued (report=%u touch=%u button=%u)\n",
                  rid, h->last_tip, h->last_button);
        }
    }
}

/* ---- device poll --------------------------------------------------------- */
static void ihid_release_contacts(i2c_hid_device_t *h)
{
    h->frame_count = 0;
    if (h->f_x >= 0 && h->fields[h->f_x].count > 1) {
        unsigned slots = h->fields[h->f_x].count;
        if (slots > I2C_HID_MAX_CONTACTS)
            slots = I2C_HID_MAX_CONTACTS;
        for (unsigned i = 0; i < slots; i++) {
            ihid_emit(h, EV_ABS, ABS_MT_SLOT, i);
            ihid_emit(h, EV_ABS, ABS_MT_TRACKING_ID, -1);
        }
    }
    ihid_emit(h, EV_KEY, BTN_TOUCH, 0);
    ihid_emit(h, EV_KEY, BTN_TOOL_FINGER, 0);
    if (h->f_button >= 0)
        ihid_emit(h, EV_KEY, BTN_LEFT, 0);
    ihid_emit(h, EV_SYN, SYN_REPORT, 0);
    if (!ihid_publish(h, 0))
        (void)ihid_publish(h, 1);
    h->last_contacts = 0;
    h->last_tip = h->last_button = 0;
}

static int ihid_recover(i2c_hid_device_t *h)
{
    h->recoveries++;
    int rc = ihid_set_power(h, I2C_HID_POWER_ON);
    if (!rc)
        rc = ihid_reset(h);
    if (!rc) {
        mdelay(60);
        rc = ihid_configure_synaptics(h);
    }
    h->recover_at = clock_get_ticks() + MS_TO_TICKS(1000);
    if (!rc)
        h->recovery_pending = 0;
    if (h->dev && (h->recoveries <= 5 || !(h->recoveries % 16)))
        kinfo("[I2C-HID] recovery attempt=%llu result=%d reporting=%s\n",
              (unsigned long long)h->recoveries, rc, rc ? "retrying" : "restored");
    return rc;
}

static int ihid_poll_device(i2c_hid_device_t *h)
{
    mutex_lock(&h->io_lock);
    if (!h->present) {
        mutex_unlock(&h->io_lock);
        return -ENODEV;
    }
    /* A failed read may consume a contact/button release or leave the slave
     * waiting for the rest of a transfer. Recover without depending on a new
     * attention edge, which a wedged device may never produce. */
    if (h->recovery_pending) {
        int rc = clock_get_ticks() >= h->recover_at ? ihid_recover(h) : 0;
        mutex_unlock(&h->io_lock);
        return rc;
    }
    if (!ihid_attention_pending(h)) {
        h->gpio_skips++;
        mutex_unlock(&h->io_lock);
        return 0;
    }
    if (h->dev && h->attention.reg && !h->attention_seen) {
        h->attention_seen = 1;
        kinfo("[I2C-HID] first GPIO attention asserted\n");
    }
    h->reads++;
    int rc = ihid_read_input_raw(h, h->input_buf, h->input_buf_len);
    if (rc == 0) {
        /* i2c_hid_get_input(): the first little-endian word is the report
         * length, not a masked field.  Zero is the reset/ack marker and
         * 0xFFFF is the "no data" value some devices return; neither is a
         * report. */
        uint16_t n = le16(h->input_buf);
        if (n > 2U && n <= h->input_buf_len) {
            h->valid_reports++;
            if (h->dev && !h->first_report_logged) {
                h->first_report_logged = 1;
                kinfo("[I2C-HID] first input report: id=%u bytes=%u\n",
                      h->uses_report_ids ? h->input_buf[2] : 0, n);
            }
            ihid_handle_report(h, h->input_buf + 2, (size_t)n - 2U);
        } else if (!n) {
            ihid_release_contacts(h);
            h->recovery_pending = 1; /* unsolicited reset completion */
            h->recover_at = 0;
        } else if (n && !h->invalid_input_logged) {
            h->invalid_input_logged = 1;
            kwarn("[I2C-HID] first invalid input length: %u (read %u)\n",
                  n, h->input_buf_len);
        }
    } else {
        h->read_errors++;
        ihid_release_contacts(h);
        h->recovery_pending = 1;
        h->recover_at = 0;
        if (h->dev && !h->poll_error_logged) {
            h->poll_error_logged = 1;
            kwarn("[I2C-HID] first input read error: %d\n", rc);
        }
    }
    mutex_unlock(&h->io_lock);
    return rc;
}

void a20_i2c_hid_start_worker(void)
{
    /* Embedded firmware probes precede the scheduler. Retry deferred clients
     * only after proc_init; runtime modules already load in task context. */
    ihid_scheduler_ready = 1;
    (void)driver_retry_deferred();
}

static void ihid_poll_work(void *context)
{
    i2c_hid_device_t *h = context;
    (void)ihid_poll_device(h);
    mutex_lock(&h->io_lock);
    if (h->running && h->present) {
        if (timer_get_ticks() >= h->report_at) {
            klog_record_only(KLOG_INFO, "[I2C-HID] runtime: gpio=%08x reads=%llu reports=%llu errors=%llu skips=%llu events=%u touch_frames=%llu move_frames=%llu drops=%llu recoveries=%llu pending=%u\n",
                h->attention.reg ? readl(h->attention.reg) : 0,
                (unsigned long long)h->reads, (unsigned long long)h->valid_reports,
                (unsigned long long)h->read_errors, (unsigned long long)h->gpio_skips,
                h->first_event_logged, (unsigned long long)h->touch_frames,
                (unsigned long long)h->movement_frames,
                (unsigned long long)h->queue_overflows,
                (unsigned long long)h->recoveries, h->recovery_pending);
            h->report_at = timer_get_ticks() + MS_TO_TICKS(30000);
        }
        (void)driver_work_schedule(h->work,
            timer_get_ticks() + MS_TO_TICKS(h->poll_ms ? h->poll_ms : 8U));
    }
    mutex_unlock(&h->io_lock);
}

/* ---- input class ops ----------------------------------------------------- */
static int ihid_input_read(device_t *dev, void *buffer, size_t count)
{
    i2c_hid_device_t *h = dev ? dev->drv_priv : NULL;
    if (!h || count < sizeof(struct input_event))
        return -EINVAL;
    size_t copied = 0;
    uint64_t flags = spin_lock_irqsave(&h->event_lock);
    while (h->tail != h->head &&
           copied + sizeof(struct input_event) <= count) {
        *(struct input_event *)((uint8_t *)buffer + copied) = h->events[h->tail];
        h->tail = (h->tail + 1U) % I2C_HID_EVENT_RING;
        copied += sizeof(struct input_event);
    }
    spin_unlock_irqrestore(&h->event_lock, flags);
    return copied ? (int)copied : -EAGAIN;
}

static int ihid_input_poll(device_t *dev, short events)
{
    (void)events;
    i2c_hid_device_t *h = dev ? dev->drv_priv : NULL;
    if (!h)
        return 0;
    uint64_t flags = spin_lock_irqsave(&h->event_lock);
    int ready = h->head != h->tail;
    spin_unlock_irqrestore(&h->event_lock, flags);
    return ready;
}

static int ihid_input_ioctl(device_t *dev, unsigned long request, void *argument)
{
    if (request != INPUT_IOCTL_GET_INFO || !argument)
        return -ENOTTY;
    i2c_hid_device_t *h = dev ? dev->drv_priv : NULL;
    if (!h)
        return -ENODEV;
    struct input_device_info *info = argument;
    memset(info, 0, sizeof(*info));
    strncpy(info->name, "I2C HID touchpad", sizeof(info->name) - 1);
    info->id.bustype = BUS_I2C;
    info->id.vendor = h->hd.vendor_id;
    info->id.product = h->hd.product_id;
    info->id.version = h->hd.version_id;
    input_info_bit(info, EV_SYN, SYN_REPORT);
    if (h->f_button >= 0)
        input_info_bit(info, EV_KEY, BTN_LEFT);
    i2c_hid_field_t *xf = h->f_x >= 0 ? &h->fields[h->f_x] : NULL;
    if (xf && xf->relative) {
        input_info_bit(info, EV_REL, REL_X);
        input_info_bit(info, EV_REL, REL_Y);
        return 0;
    }
    input_info_bit(info, EV_KEY, BTN_TOUCH);
    input_info_bit(info, EV_KEY, BTN_TOOL_FINGER);
    info->properties[0] = 1; /* INPUT_PROP_POINTER */
    if (h->hd.vendor_id == SYNA_VENDOR_ID && h->hd.product_id == SYNA_CE78_PRODUCT_ID)
        info->properties[0] |= 1U << 2; /* INPUT_PROP_BUTTONPAD */
    if (h->f_x >= 0 && h->fields[h->f_x].count > 1) {
        /* The slot protocol and legacy pointer axes share the same units. */
        i2c_hid_field_t *fx = &h->fields[h->f_x];
        i2c_hid_field_t *fy = h->f_y >= 0 ? &h->fields[h->f_y] : NULL;
        input_info_bit(info, EV_ABS, ABS_MT_SLOT);
        input_info_bit(info, EV_ABS, ABS_MT_TRACKING_ID);
        input_info_bit(info, EV_ABS, ABS_MT_POSITION_X);
        input_info_bit(info, EV_ABS, ABS_MT_POSITION_Y);
        info->abs[ABS_MT_SLOT].minimum = 0;
        info->abs[ABS_MT_SLOT].maximum =
            (int32_t)(fx->count ? fx->count - 1U : 0U);
        info->abs[ABS_MT_TRACKING_ID].minimum = 0;
        info->abs[ABS_MT_TRACKING_ID].maximum = 65535;
        info->abs[ABS_MT_TRACKING_ID].value = -1;
        info->abs[ABS_MT_POSITION_X].minimum = fx->logical_min;
        info->abs[ABS_MT_POSITION_X].maximum = fx->logical_max;
        info->abs[ABS_MT_POSITION_X].resolution = h->axis_resolution;
        if (fy) {
            info->abs[ABS_MT_POSITION_Y].minimum = fy->logical_min;
            info->abs[ABS_MT_POSITION_Y].maximum = fy->logical_max;
            info->abs[ABS_MT_POSITION_Y].resolution = h->axis_resolution;
        }
    }
    if (h->f_x >= 0) {
        i2c_hid_field_t *f = &h->fields[h->f_x];
        input_info_bit(info, EV_ABS, ABS_X);
        info->abs[ABS_X].minimum = f->logical_min;
        info->abs[ABS_X].maximum = f->logical_max;
        info->abs[ABS_X].resolution = h->axis_resolution;
    }
    if (h->f_y >= 0) {
        i2c_hid_field_t *f = &h->fields[h->f_y];
        input_info_bit(info, EV_ABS, ABS_Y);
        info->abs[ABS_Y].minimum = f->logical_min;
        info->abs[ABS_Y].maximum = f->logical_max;
        info->abs[ABS_Y].resolution = h->axis_resolution;
    }
    return 0;
}

static const input_dev_ops_t ihid_input_ops = {
    .read = ihid_input_read,
    .ioctl = ihid_input_ioctl,
    .poll = ihid_input_poll,
};

/* ---- firmware plumbing --------------------------------------------------- */
static uint16_t g_ihid_boot_addr;
static uint32_t g_ihid_boot_poll_ms;

static void ihid_parse_cmdline(void)
{
    const char *cmdline = bootargs_get();
    for (const char *p = cmdline; p && *p; ) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        const char *end = p;
        while (*end && *end != ' ' && *end != '\t')
            end++;
        static const char addr_key[] = "a20.i2chid.addr=";
        static const char poll_key[] = "a20.i2chid.poll_ms=";
        if ((size_t)(end - p) > sizeof(addr_key) - 1 &&
            !memcmp(p, addr_key, sizeof(addr_key) - 1)) {
            uint32_t v = 0;
            for (const char *q = p + sizeof(addr_key) - 1; q < end; q++) {
                uint32_t digit;
                if (*q >= '0' && *q <= '9') digit = (uint32_t)(*q - '0');
                else if (*q >= 'a' && *q <= 'f') digit = (uint32_t)(*q - 'a' + 10);
                else if (*q >= 'A' && *q <= 'F') digit = (uint32_t)(*q - 'A' + 10);
                else break;
                v = v * 16U + digit;
            }
            if (v > 0 && v < 0x80U)
                g_ihid_boot_addr = (uint16_t)v;
        } else if ((size_t)(end - p) > sizeof(poll_key) - 1 &&
                   !memcmp(p, poll_key, sizeof(poll_key) - 1)) {
            uint32_t v = 0;
            for (const char *q = p + sizeof(poll_key) - 1;
                 q < end && *q >= '0' && *q <= '9'; q++)
                v = v * 10U + (uint32_t)(*q - '0');
            if (v >= 2U && v <= 100U)
                g_ihid_boot_poll_ms = v;
        }
        p = end;
    }
}

static int ihid_probe_descriptor(i2c_hid_device_t *h, const fw_node_t *node)
{
    uint32_t addr = g_ihid_boot_addr;
    if (!addr) {
        int rc = fw_node_read_u32(node, "acpi,i2c-address", &addr);
        if (rc || !addr || addr > 0x7fU) {
            kerr("[I2C-HID] adapter %d: no valid 7-bit I2C address in _CRS (%d)\n",
                 h->adapter->nr, rc);
            return rc ? rc : -EINVAL;
        }
    }
    uint32_t reg = I2C_HID_DESC_REG;
    int rc = fw_node_read_u32(node, "acpi,hid-descriptor-register", &reg);
    if ((rc && rc != -ENOENT) || reg > 0xffffU) {
        kerr("[I2C-HID] adapter %d addr=%02x: _DSM HID descriptor register failed (%d)\n",
             h->adapter->nr, addr, rc);
        return rc ? rc : -EINVAL;
    }
    uint32_t speed = 0;
    (void)fw_node_read_u32(node, "acpi,i2c-speed", &speed);
    h->addr = (uint16_t)addr;
    kinfo("[I2C-HID] adapter %d addr=%02x speed=%u descriptor=%04x (%s)\n",
          h->adapter->nr, addr, speed, reg, rc ? "default" : "_DSM");
    rc = ihid_fetch_descriptor(h, (uint16_t)reg);
    if (rc)
        kerr("[I2C-HID] adapter %d addr=%02x descriptor=%04x read failed (%d)\n",
             h->adapter->nr, addr, reg, rc);
    return rc;
}

static int ihid_probe(device_t *dev)
{
#ifdef CONFIG_DRIVER_MODULE
    ihid_scheduler_ready = 1;
#endif
    if (!ihid_scheduler_ready)
        return -EPROBE_DEFER;
    ihid_parse_cmdline();

    i2c_adapter_t *adapter = i2c_adapter_get_for_device(dev->parent);
    if (!adapter)
        return -EPROBE_DEFER;

    int error = device_add_dependency(dev, dev->parent);
    if (error) { i2c_adapter_put(adapter); return error; }
    i2c_hid_device_t *h = kcalloc(1, sizeof(*h));
    if (!h) { i2c_adapter_put(adapter); return -ENOMEM; }
    h->dev = dev;
    h->adapter = adapter;
    h->poll_ms = g_ihid_boot_poll_ms ? g_ihid_boot_poll_ms : 8U;
    spin_init(&h->event_lock);
    mutex_init(&h->io_lock);
    h->f_x = h->f_y = h->f_tip = h->f_button = h->f_contact_id = -1;

    int probe_rc = ihid_probe_descriptor(h, &dev->firmware_node);
    if (probe_rc) { error = probe_rc; goto fail; }

    h->report_desc = kmalloc(I2C_HID_MAX_REPORT_DESC);
    h->input_buf = kmalloc(I2C_HID_MAX_INPUT);
    if (!h->report_desc || !h->input_buf) {
        error = -ENOMEM;
        goto fail;
    }
    /* Linux reads the report descriptor only after powering the device on
     * (i2c_hid_parse()): a sleeping device NAKs the descriptor read. */
    if (ihid_set_power(h, I2C_HID_POWER_ON) < 0)
        kinfo("[I2C-HID] power-on command failed; continuing\n");
    mdelay(60); /* Goodix/Windows settle time from i2c_hid_set_power() */

    uint16_t rd_len = h->hd.report_desc_length;
    if (rd_len > I2C_HID_MAX_REPORT_DESC)
        rd_len = I2C_HID_MAX_REPORT_DESC;
    h->hd.report_desc_length = rd_len;
    if (ihid_read_reg(h, h->hd.report_desc_register, h->report_desc,
                      rd_len) < 0) {
        kerr("[I2C-HID] report descriptor read failed\n");
        error = -EIO;
        goto fail;
    }
    h->input_buf_len = h->hd.max_input_length;

    hid_parse_report_descriptor(h);
    if (ihid_choose_report(h) < 0) {
        kerr("[I2C-HID] report descriptor has no pointer report\n");
        error = -ENODEV;
        goto fail;
    }

    /* Size the input buffer from the parsed descriptor as well as the
     * descriptor's own wMaxInputLength: the latter is wrong on some devices
     * (Linux i2c_hid_find_max_report() exists for exactly that). */
    uint16_t need = (uint16_t)(2U + h->max_report_bytes + h->uses_report_ids);
    if (need < 2U)
        need = 2U;
    if (need > I2C_HID_MAX_INPUT)
        need = I2C_HID_MAX_INPUT;
    if (need > h->input_buf_len)
        h->input_buf_len = need;
    if (h->input_buf_len < 2U)
        h->input_buf_len = 2U;

    ihid_setup_attention(h, &dev->firmware_node);
    /* The descriptor advertises the length expected for an input read. */
    if (h->attention.reg && h->hd.max_input_length >= 2)
        h->input_buf_len = h->hd.max_input_length;
    int reset_rc = ihid_reset(h);
    if (h->attention.reg || reset_rc)
        kinfo("[I2C-HID] reset result=%d input read=%u\n", reset_rc, h->input_buf_len);
    int wake_rc = ihid_set_power(h, I2C_HID_POWER_ON);
    if (wake_rc)
        kwarn("[I2C-HID] wake after reset failed (%d)\n", wake_rc);
    mdelay(60);
    int mode_rc = ihid_configure_synaptics(h);
    if (mode_rc)
        kwarn("[I2C-HID] Synaptics touch mode failed (%d)\n", mode_rc);
    else if (h->hd.vendor_id == SYNA_VENDOR_ID &&
             h->hd.product_id == SYNA_CE78_PRODUCT_ID &&
             h->active_report_id == SYNA_CE78_TOUCH_REPORT)
    {
        kinfo("[I2C-HID] Synaptics touch mode and reporting switches set\n");
        int verify_rc = ihid_synaptics_check_features(h);
        if (verify_rc)
            kwarn("[I2C-HID] feature readback failed (%d)\n", verify_rc);
    }

    h->present = 1;
    h->running = 1;
    h->queue = driver_workqueue_create();
    if (!h->queue) { error = -ENOMEM; goto fail; }
    h->work = driver_work_create(h->queue, ihid_poll_work, h);
    if (!h->work) {
        (void)driver_workqueue_destroy(&h->queue, 0);
        error = -ENOMEM;
        goto fail;
    }
    h->report_at = timer_get_ticks() + MS_TO_TICKS(10000);
    dev->drv_priv = h;

    kinfo("[I2C-HID] ready: addr=0x%02x vid=0x%04x pid=0x%04x "
          "report=%u fields=%u poll=%ums%s\n",
          h->addr, h->hd.vendor_id, h->hd.product_id, h->active_report_id,
          h->field_count, (unsigned)h->poll_ms,
          h->f_tip >= 0 ? " touch" : " mouse");
    (void)driver_work_schedule(h->work, 0);
    kinfo("[I2C-HID] polling worker started\n");
    return 0;
fail:
    kfree(h->report_desc);
    kfree(h->input_buf);
    i2c_adapter_put(adapter);
    kfree(h);
    return error;
}

static int ihid_remove(device_t *dev)
{
    i2c_hid_device_t *h = dev ? dev->drv_priv : NULL;
    if (!h)
        return 0;
    mutex_lock(&h->io_lock);
    h->present = 0;
    h->running = 0;
    mutex_unlock(&h->io_lock);
    /* Close submissions and wait outside io_lock: the callback takes it.
     * A failed teardown retains context, work and adapter for a safe retry. */
    int ret = driver_workqueue_destroy(&h->queue,
                                       timer_get_ticks() + MS_TO_TICKS(2000));
    if (ret)
        return ret;
    driver_work_put(h->work);
    i2c_adapter_put(h->adapter);
    dev->drv_priv = NULL;
    kfree(h->report_desc);
    kfree(h->input_buf);
    kfree(h);
    return 0;
}

/*
 * Parser self-test.
 *
 * The report-descriptor parser is the one part of this driver that can be
 * verified without the hardware: a hand-assembled descriptor with known field
 * positions, and a report whose bytes encode known values, pins down the bit
 * arithmetic.  It runs from the boot self-test hook and never touches the bus.
 *
 * Descriptor (report ID 1): tip switch at bit 0, 7 bits of padding, X at bit
 * 8 (16 bit), Y at bit 24 (16 bit), button 1 at bit 40, 7 bits padding.
 */
static int ihid_parser_selftest(void)
{
    static const uint8_t desc[] = {
        0x05, 0x0D,             /* Usage Page (Digitizer) */
        0x09, 0x05,             /* Usage (Touch Pad) */
        0xA1, 0x01,             /* Collection (Application) */
        0x85, 0x01,             /*   Report ID (1) */
        0x09, 0x42,             /*   Usage (Tip Switch) */
        0x15, 0x00,             /*   Logical Minimum (0) */
        0x25, 0x01,             /*   Logical Maximum (1) */
        0x75, 0x01,             /*   Report Size (1) */
        0x95, 0x01,             /*   Report Count (1) */
        0x81, 0x02,             /*   Input (Data,Var,Abs) */
        0x75, 0x07,             /*   Report Size (7) */
        0x95, 0x01,             /*   Report Count (1) */
        0x81, 0x03,             /*   Input (Const) */
        0x05, 0x01,             /*   Usage Page (Generic Desktop) */
        0x09, 0x30,             /*   Usage (X) */
        0x09, 0x31,             /*   Usage (Y) */
        0x27, 0xFF, 0x7F, 0x00, 0x00, /* Logical Maximum (32767) */
        0x75, 0x10,             /*   Report Size (16) */
        0x95, 0x02,             /*   Report Count (2) */
        0x81, 0x02,             /*   Input (Data,Var,Abs) */
        0x05, 0x09,             /*   Usage Page (Button) */
        0x19, 0x01,             /*   Usage Minimum (1) */
        0x29, 0x01,             /*   Usage Maximum (1) */
        0x15, 0x00,             /*   Logical Minimum (0) */
        0x25, 0x01,             /*   Logical Maximum (1) */
        0x75, 0x01,             /*   Report Size (1) */
        0x95, 0x01,             /*   Report Count (1) */
        0x81, 0x02,             /*   Input (Data,Var,Abs) */
        0x75, 0x07,             /*   Report Size (7) */
        0x95, 0x01,             /*   Report Count (1) */
        0x81, 0x03,             /*   Input (Const) */
        0xC0,                   /* End Collection */
    };
    /* Report ID 1, tip=1, X=1000 (0x03e8), Y=2000 (0x07d0), button=1. */
    static const uint8_t report[] = {
        0x01, 0x01, 0xE8, 0x03, 0xD0, 0x07, 0x01, 0x00,
    };
    static i2c_hid_device_t t;
    memset(&t, 0, sizeof(t));
    t.report_desc = (uint8_t *)(uintptr_t)desc;
    t.hd.report_desc_length = sizeof(desc);
    t.f_x = t.f_y = t.f_tip = t.f_button = t.f_contact_id = -1;

    hid_parse_report_descriptor(&t);
    if (ihid_choose_report(&t) < 0)
        return -1;
    if (t.active_report_id != 1 || t.f_tip != 0)
        return -1;
    i2c_hid_field_t *fx = &t.fields[t.f_x];
    i2c_hid_field_t *fy = &t.fields[t.f_y];
    i2c_hid_field_t *fb = t.f_button >= 0 ? &t.fields[t.f_button] : NULL;
    if (fx->bit_offset != 8 || fx->bit_size != 16 || fx->logical_max != 32767)
        return -1;
    if (fy->bit_offset != 24 || fy->bit_size != 16)
        return -1;
    if (!fb || fb->bit_offset != 40 || fb->bit_size != 1)
        return -1;

    const uint8_t *body = report + 1; /* skip the report id */
    size_t body_len = sizeof(report) - 1;
    if (hid_extract(body, body_len, &t.fields[t.f_tip]) != 1)
        return -1;
    if ((int32_t)hid_extract(body, body_len, fx) != 1000)
        return -1;
    if ((int32_t)hid_extract(body, body_len, fy) != 2000)
        return -1;
    if ((int32_t)hid_extract(body, body_len, fb) != 1)
        return -1;
    return 0;
}

/*
 * Protocol self-test.
 *
 * The rest of the driver only runs when the hardware is present, so this
 * exercises the same code against a mock I2C adapter: the HID descriptor and
 * report descriptor reads, the command framing, the plain input-register
 * read, and the event that reaches the input queue.  Nothing here touches a
 * real bus; a mock transfer validates the framing the way a device would.
 */
typedef struct ihid_mock {
    uint8_t desc[30];
    const uint8_t *report_desc;
    uint16_t report_desc_len;
    const uint8_t *input;
    uint16_t input_len;
    uint8_t last_command[8];
    uint8_t last_command_len;
    uint16_t report_desc_register;
    uint16_t descriptor_register;
    int desc_reads;
    uint8_t feature_values[16];
    uint16_t feature_written;
    unsigned fail_reads;
    uint8_t reset_ack_enabled, reset_ack;
} ihid_mock_t;

static int ihid_mock_transfer(i2c_adapter_t *adapter, i2c_msg_t *msgs, int n)
{
    ihid_mock_t *m = adapter->priv;
    if (!m || n <= 0 || !msgs)
        return -EINVAL;

    if (n == 2 && !(msgs[0].flags & I2C_M_READ) &&
        (msgs[1].flags & I2C_M_READ)) {
        if (msgs[0].len == 6 && msgs[0].buf[3] == I2C_HID_CMD_GET_REPORT &&
            msgs[1].len == 4) {
            unsigned id = msgs[0].buf[2] & 15;
            msgs[1].buf[0] = 4;
            msgs[1].buf[1] = 0;
            msgs[1].buf[2] = id;
            msgs[1].buf[3] = m->feature_values[id];
            return 0;
        }
        if (msgs[0].len != 2)
            return -EINVAL;
        uint16_t reg = (uint16_t)(msgs[0].buf[0] | (msgs[0].buf[1] << 8));
        const uint8_t *src;
        uint16_t len;
        if (reg == m->descriptor_register) {
            src = m->desc;
            len = sizeof(m->desc);
            m->desc_reads++;
        } else if (reg == m->report_desc_register) {
            src = m->report_desc;
            len = m->report_desc_len;
        } else {
            return -ENXIO;
        }
        uint16_t copy = msgs[1].len < len ? msgs[1].len : len;
        memcpy(msgs[1].buf, src, copy);
        return copy == msgs[1].len ? 0 : -EIO;
    }
    if (n == 1 && (msgs[0].flags & I2C_M_READ)) {
        if (m->fail_reads) {
            m->fail_reads--;
            return -ETIMEDOUT;
        }
        if (m->reset_ack) {
            memset(msgs[0].buf, 0, msgs[0].len);
            m->reset_ack = 0;
            return 0;
        }
        uint16_t copy = msgs[0].len < m->input_len ? msgs[0].len : m->input_len;
        memcpy(msgs[0].buf, m->input, copy);
        if (copy < msgs[0].len)
            memset(msgs[0].buf + copy, 0, msgs[0].len - copy);
        return 0;
    }
    if (n == 1 && !(msgs[0].flags & I2C_M_READ)) {
        /* Register-prefixed command write: remember the payload after the
         * two address bytes so the test can check the framing. */
        uint16_t payload = msgs[0].len > 2 ? (uint16_t)(msgs[0].len - 2U) : 0;
        if (payload > sizeof(m->last_command))
            payload = sizeof(m->last_command);
        if (payload)
            memcpy(m->last_command, msgs[0].buf + 2, payload);
        m->last_command_len = (uint8_t)payload;
        if (m->reset_ack_enabled && payload == 2 &&
            m->last_command[1] == I2C_HID_CMD_RESET)
            m->reset_ack = 1;
        if (payload == 8 && (m->last_command[0] >> 4) == 3 &&
            m->last_command[1] == I2C_HID_CMD_SET_REPORT) {
            unsigned id = m->last_command[0] & 15;
            m->feature_values[id] = m->last_command[7];
            m->feature_written |= (uint16_t)(1U << id);
        }
        return 0;
    }
    return -EINVAL;
}

static const uint8_t ihid_test_report_desc[] = {
    0x05, 0x0D, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x01,
    0x09, 0x42, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
    0x95, 0x01, 0x81, 0x02, 0x75, 0x07, 0x95, 0x01,
    0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x27, 0xFF, 0x7F, 0x00, 0x00, 0x75, 0x10, 0x95, 0x02, 0x81, 0x02,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x01, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x01, 0x81, 0x02, 0x75, 0x07, 0x95, 0x01, 0x81, 0x03,
    0xC0,
};

/* Length word + report id 1 + tip + X=1000 + Y=2000 + button 1.  The length
 * covers the whole transfer including its own two bytes, which is what
 * i2c_hid_format_report() writes and i2c_hid_get_input() subtracts. */
static const uint8_t ihid_test_input[] = {
    0x0A, 0x00,
    0x01, 0x01, 0xE8, 0x03, 0xD0, 0x07, 0x01, 0x00,
};

static int ihid_protocol_selftest(void)
{
    static i2c_hid_device_t t;
    static ihid_mock_t mock;
    static i2c_adapter_t adapter;

    memset(&t, 0, sizeof(t));
    memset(&mock, 0, sizeof(mock));
    memset(&adapter, 0, sizeof(adapter));

    mock.report_desc = ihid_test_report_desc;
    mock.report_desc_len = sizeof(ihid_test_report_desc);
    mock.input = ihid_test_input;
    mock.input_len = sizeof(ihid_test_input);
    mock.report_desc_register = 0x0010;
    mock.descriptor_register = 0x0020;
    mock.desc[0] = 30;      /* wHIDDescLength */
    mock.desc[2] = 0x00;    /* bcdVersion = 0x0100 */
    mock.desc[3] = 0x01;
    mock.desc[4] = (uint8_t)(sizeof(ihid_test_report_desc) & 0xff);
    mock.desc[5] = (uint8_t)(sizeof(ihid_test_report_desc) >> 8);
    mock.desc[6] = 0x10;    /* wReportDescRegister = 0x0010 */
    mock.desc[7] = 0x00;
    mock.desc[8] = 0x03;    /* wInputRegister (unused by the mock read path) */
    mock.desc[10] = (uint8_t)sizeof(ihid_test_input);
    mock.desc[16] = 0x05;   /* wCommandRegister = 0x0005 */
    mock.desc[18] = 0x06;   /* wDataRegister = 0x0006 */
    mock.desc[20] = 0x6b;   /* vendor 0x056a (Synaptics) */
    mock.desc[21] = 0x05;

    adapter.name = "mock";
    adapter.nr = 0;
    adapter.transfer = ihid_mock_transfer;
    adapter.priv = &mock;

    t.adapter = &adapter;
    t.addr = 0x2c;
    t.f_x = t.f_y = t.f_tip = t.f_button = t.f_contact_id = -1;

    if (ihid_fetch_descriptor(&t, 0x0020) < 0 || mock.desc_reads == 0)
        return -1;
    if (ihid_set_power(&t, I2C_HID_POWER_ON) < 0)
        return -1;
    /* SET_POWER framing: [(0<<4)|state, opcode]. */
    if (mock.last_command_len != 2 || mock.last_command[0] != 0x00 ||
        mock.last_command[1] != I2C_HID_CMD_SET_POWER)
        return -1;
    if (ihid_command(&t, 0, 0, I2C_HID_CMD_RESET) < 0 ||
        mock.last_command_len != 2 || mock.last_command[0] != 0x00 ||
        mock.last_command[1] != I2C_HID_CMD_RESET)
        return -1;
    static const uint8_t mode[] = {0x36, 0x03, 0x06, 0, 4, 0, 6, 3};
    if (ihid_synaptics_set_touch_mode(&t) < 0 ||
        mock.feature_written != ((1U << 4) | (1U << 13) | (1U << 6)) ||
        mock.feature_values[4] != 3 || mock.feature_values[13] != 0 ||
        mock.feature_values[6] != 3 ||
        mock.last_command_len != sizeof(mode) ||
        memcmp(mock.last_command, mode, sizeof(mode)))
        return -1;
    if (ihid_synaptics_check_features(&t))
        return -1;
    mock.feature_values[6] = 0;
    if (ihid_synaptics_check_features(&t) != -EIO)
        return -1;

    t.report_desc = kmalloc(I2C_HID_MAX_REPORT_DESC);
    t.input_buf = kmalloc(I2C_HID_MAX_INPUT);
    if (!t.report_desc || !t.input_buf)
        return -1;
    if (ihid_read_reg(&t, t.hd.report_desc_register, t.report_desc,
                      t.hd.report_desc_length) < 0)
        return -1;
    hid_parse_report_descriptor(&t);
    if (ihid_choose_report(&t) < 0 || t.active_report_id != 1)
        return -1;

    t.present = 1;
    t.input_buf_len = sizeof(ihid_test_input);
    uint32_t attention = 1U;
    t.attention.reg = &attention;
    t.attention.mask = 1U;
    t.attention.active_low = 1;
    if (ihid_poll_device(&t) != 0 || t.head != t.tail)
        return -1;
    attention = 0;
    if (ihid_poll_device(&t) != 0)
        return -1;

    /* ihid_handle_report() queued BTN_TOOL_FINGER, BTN_TOUCH, ABS_X,
     * ABS_Y, BTN_LEFT and SYN_REPORT. */
    int saw_x = 0, saw_y = 0, saw_touch = 0, saw_button = 0, saw_syn = 0;
    int32_t x = 0, y = 0;
    while (t.tail != t.head) {
        struct input_event *e = &t.events[t.tail];
        if (e->type == EV_ABS && e->code == ABS_X) {
            saw_x = 1;
            x = e->value;
        } else if (e->type == EV_ABS && e->code == ABS_Y) {
            saw_y = 1;
            y = e->value;
        } else if (e->type == EV_KEY && e->code == BTN_TOUCH && e->value == 1) {
            saw_touch = 1;
        } else if (e->type == EV_KEY && e->code == BTN_LEFT && e->value == 1) {
            saw_button = 1;
        } else if (e->type == EV_SYN && e->code == SYN_REPORT) {
            saw_syn = 1;
        }
        t.tail = (t.tail + 1U) % I2C_HID_EVENT_RING;
    }

    kfree(t.report_desc);
    kfree(t.input_buf);
    t.report_desc = NULL;
    t.input_buf = NULL;

    if (!saw_x || !saw_y || !saw_touch || !saw_button || !saw_syn)
        return -1;
    if (x != 1000 || y != 2000)
        return -1;
    return 0;
}

/* Precision-touchpad variant of the protocol test: two contacts in one
 * report, exercising the ABS_MT slot path. */
static const uint8_t ihid_mt_report_desc[] = {
    0x05, 0x0D, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x01,
    0x09, 0x42, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02,
    0x75, 0x06, 0x95, 0x01, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x27, 0xFF, 0x7F, 0x00, 0x00,
    0x75, 0x10, 0x95, 0x02, 0x81, 0x02,
    0x09, 0x31, 0x95, 0x02, 0x81, 0x02,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x01, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x01, 0x81, 0x02, 0x75, 0x07, 0x95, 0x01, 0x81, 0x03,
    0xC0,
};

static const uint8_t ihid_mt_input[] = {
    0x0D, 0x00,             /* whole transfer = 13 bytes */
    0x01,                   /* report id */
    0x03,                   /* both tip switches down */
    0x64, 0x00,             /* contact 0 X = 100 */
    0xC8, 0x00,             /* contact 1 X = 200 */
    0x2C, 0x01,             /* contact 0 Y = 300 */
    0x90, 0x01,             /* contact 1 Y = 400 */
    0x01,                   /* button 1 */
};

static int ihid_mt_selftest(void)
{
    static i2c_hid_device_t t;
    static ihid_mock_t mock;
    static i2c_adapter_t adapter;

    memset(&t, 0, sizeof(t));
    memset(&mock, 0, sizeof(mock));
    memset(&adapter, 0, sizeof(adapter));
    mock.report_desc = ihid_mt_report_desc;
    mock.report_desc_len = sizeof(ihid_mt_report_desc);
    mock.input = ihid_mt_input;
    mock.input_len = sizeof(ihid_mt_input);
    mock.report_desc_register = 0x0010;
    mock.descriptor_register = I2C_HID_DESC_REG;
    mock.desc[0] = 30;
    mock.desc[3] = 0x01;
    mock.desc[4] = (uint8_t)(sizeof(ihid_mt_report_desc) & 0xff);
    mock.desc[5] = (uint8_t)(sizeof(ihid_mt_report_desc) >> 8);
    mock.desc[6] = 0x10;
    mock.desc[8] = 0x03;  /* wInputRegister */
    mock.desc[10] = (uint8_t)sizeof(ihid_mt_input);
    mock.desc[16] = 0x05;
    adapter.name = "mock";
    adapter.transfer = ihid_mock_transfer;
    adapter.priv = &mock;

    t.adapter = &adapter;
    t.addr = 0x2c;
    t.f_x = t.f_y = t.f_tip = t.f_button = t.f_contact_id = -1;
    if (ihid_fetch_descriptor(&t, I2C_HID_DESC_REG) < 0)
        return -1;
    t.report_desc = kmalloc(I2C_HID_MAX_REPORT_DESC);
    t.input_buf = kmalloc(I2C_HID_MAX_INPUT);
    if (!t.report_desc || !t.input_buf)
        return -1;
    if (ihid_read_reg(&t, t.hd.report_desc_register, t.report_desc,
                      t.hd.report_desc_length) < 0)
        return -1;
    hid_parse_report_descriptor(&t);
    if (ihid_choose_report(&t) < 0 || t.active_report_id != 1)
        return -1;
    if (t.f_x < 0 || t.fields[t.f_x].count != 2)
        return -1;

    t.present = 1;
    t.input_buf_len = sizeof(ihid_mt_input);
    if (ihid_poll_device(&t) != 0)
        return -1;

    int slots = 0, ids = 0, tool = 0;
    int32_t xs[2] = {-1, -1}, ys[2] = {-1, -1};
    unsigned slot = 0;
    while (t.tail != t.head) {
        struct input_event *e = &t.events[t.tail];
        if (e->type == EV_ABS && e->code == ABS_MT_SLOT) {
            slot = (unsigned)e->value;
            slots++;
        } else if (e->type == EV_ABS && e->code == ABS_MT_TRACKING_ID &&
                   e->value > 0) {
            ids++;
        } else if (e->type == EV_ABS && e->code == ABS_MT_POSITION_X &&
                   slot < 2) {
            xs[slot] = e->value;
        } else if (e->type == EV_ABS && e->code == ABS_MT_POSITION_Y &&
                   slot < 2) {
            ys[slot] = e->value;
        } else if (e->type == EV_KEY && e->code == BTN_TOOL_FINGER &&
                   e->value == 1) {
            tool = 1;
        }
        t.tail = (t.tail + 1U) % I2C_HID_EVENT_RING;
    }

    if (slots < 2 || ids != 2 || !tool)
        return -1;
    if (xs[0] != 100 || xs[1] != 200 || ys[0] != 300 || ys[1] != 400)
        return -1;

    /* Delay all readers long enough to overflow the source queue. The first
     * replacement frame must advertise loss AND restore active tracking ids. */
    for (unsigned i = 0; i < 100; i++)
        if (ihid_poll_device(&t))
            return -1;
    int dropped = 0, restored = 0;
    for (unsigned i = t.tail; i != t.head; i = (i + 1) % I2C_HID_EVENT_RING) {
        struct input_event *e = &t.events[i];
        if (e->type == EV_SYN && e->code == SYN_DROPPED)
            dropped = 1;
        if (dropped && e->type == EV_ABS && e->code == ABS_MT_TRACKING_ID && e->value > 0)
            restored++;
    }
    if (!t.queue_overflows || !dropped || restored < 2)
        return -1;
    t.tail = t.head;

    /* Specifically overflow on button-up, the transition a modal dialog must
     * receive even when opening its window delayed the input consumer. */
    uint8_t up[sizeof(ihid_mt_input)];
    memcpy(up, ihid_mt_input, sizeof(up));
    up[3] = 0;
    up[sizeof(up) - 1] = 0;
    mock.input = up;
    t.head = (t.tail + I2C_HID_EVENT_RING - 2) % I2C_HID_EVENT_RING;
    if (ihid_poll_device(&t) || t.last_tip || t.last_button || t.last_contacts)
        return -1;
    int button_up = 0, inactive = 0;
    dropped = 0;
    for (unsigned i = t.tail; i != t.head; i = (i + 1) % I2C_HID_EVENT_RING) {
        struct input_event *e = &t.events[i];
        if (e->type == EV_SYN && e->code == SYN_DROPPED)
            dropped = 1;
        if (e->type == EV_KEY && e->code == BTN_LEFT && !e->value)
            button_up = 1;
        if (e->type == EV_ABS && e->code == ABS_MT_TRACKING_ID && e->value == -1)
            inactive++;
    }
    if (!dropped || !button_up || inactive != 2)
        return -1;
    t.tail = t.head;
    mock.input = ihid_mt_input;
    if (ihid_poll_device(&t) || !t.last_tip || !t.last_button)
        return -1;
    t.tail = t.head;

    /* A partial I2C read loses button-up: release locally, reset/reconfigure
     * without a GPIO edge, then accept a fresh contact on the same node. */
    mock.fail_reads = 1;
    mock.reset_ack_enabled = 1;
    if (ihid_poll_device(&t) != -ETIMEDOUT || !t.recovery_pending ||
        t.last_tip || t.last_button || t.last_contacts)
        return -1;
    if (ihid_poll_device(&t) || t.recovery_pending || t.recoveries != 1)
        return -1;
    if (ihid_poll_device(&t) || !t.last_tip || !t.last_button || t.last_contacts != 3)
        return -1;
    kinfo("[I2C-HID] overflow and timeout recovery self-test: PASS\n");
    kfree(t.report_desc);
    kfree(t.input_buf);
    t.report_desc = NULL;
    t.input_buf = NULL;
    return 0;
}

/* Boot hook (see main.c): runs once at init_kthread, no bus access.  Returns
 * 0 only when all three parts pass; each part still logs its own line so a
 * failing gate names the broken layer. */
int a20_boot_selftest_i2c_hid(void)
{
    int ok = 1;
    if (ihid_parser_selftest() == 0)
        kinfo("[I2C-HID] parser self-test: PASS\n");
    else {
        kerr("[I2C-HID] parser self-test: FAIL\n");
        ok = 0;
    }
    if (ihid_protocol_selftest() == 0)
        kinfo("[I2C-HID] protocol self-test: PASS\n");
    else {
        kerr("[I2C-HID] protocol self-test: FAIL\n");
        ok = 0;
    }
    if (ihid_mt_selftest() == 0)
        kinfo("[I2C-HID] multitouch self-test: PASS\n");
    else {
        kerr("[I2C-HID] multitouch self-test: FAIL\n");
        ok = 0;
    }
    return ok ? 0 : -1;
}

static const firmware_device_id_t ihid_ids[] = {
    {.id = "PNP0C50"},   /* HID-over-I2C, generic ACPI id */
    {.id = "SYNA2BA6"},  /* Synaptics touchpad on the target laptop */
    {.id = "SYNA8000"},
    {.id = "SYNA3602"},
    {.id = NULL},
};

static const driver_t ihid_driver = {
    .name = "i2c-hid",
    .bus = &platform_bus,
    .firmware_ids = ihid_ids,
    .probe = ihid_probe,
    .remove = ihid_remove,
    .class_ops = &ihid_input_ops,
    .class_type = DEV_CLASS_INPUT,
};

DRIVER_REGISTER(ihid_driver);
