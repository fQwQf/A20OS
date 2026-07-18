/*
 * Touch calibration solver + persistence. Pure integer math (no register
 * access) so it builds and runs on the host unit test and the QEMU self-test.
 * See touch_cal.h for the model.
 */
#ifdef CONFIG_BOARD_STM32F103

#include "drivers/stm32f1/touch_cal.h"

#define TOUCH_CAL_MAGIC0 0xCAu
#define TOUCH_CAL_MAGIC1 0x1Bu
#define TOUCH_CAL_VERSION 1u

static uint16_t touch_crc16(const uint8_t *data, unsigned len) {
    uint16_t crc = 0xFFFFu;
    for (unsigned i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
    }
    return crc;
}

/*
 * Mean raw value (and, optionally, mean screen coordinate) over the points on
 * the "low" or "high" side of a screen-axis split. pick_x selects the raw axis
 * to average (1 = raw_x, 0 = raw_y); use_screen_x selects the screen axis to
 * split on; high selects which side. Returns the raw mean; *screen_mean gets
 * the mean of the split screen axis, *n_out the sample count.
 */
static uint32_t side_mean(const touch_cal_point_t *points, unsigned count,
                          int pick_x, int use_screen_x, uint16_t split,
                          int high, uint32_t *screen_mean, unsigned *n_out) {
    uint32_t raw_sum = 0, screen_sum = 0;
    unsigned n = 0;
    for (unsigned i = 0; i < count; i++) {
        uint16_t s = use_screen_x ? points[i].screen_x : points[i].screen_y;
        int is_high = s > split;
        if (is_high != high)
            continue;
        raw_sum += pick_x ? points[i].raw_x : points[i].raw_y;
        screen_sum += s;
        n++;
    }
    if (n_out)
        *n_out = n;
    if (screen_mean)
        *screen_mean = n ? screen_sum / n : 0;
    return n ? raw_sum / n : 0;
}

static uint32_t abs_diff(uint32_t a, uint32_t b) {
    return a > b ? a - b : b - a;
}

/*
 * Fit a line raw = f(screen) through the two group means and extrapolate the
 * raw values at screen coordinates 0 and (pixels-1) — the panel edges — even
 * when the calibration targets were inset corners. Emits min/max raw (clamped
 * to a valid ADC range) and the invert flag, matching the driver's scale.
 */
static int fit_axis(uint32_t raw_lo, uint32_t screen_lo, uint32_t raw_hi,
                    uint32_t screen_hi, uint16_t pixels, uint16_t *min_out,
                    uint16_t *max_out, int *invert_out) {
    if (screen_hi <= screen_lo || raw_lo == raw_hi)
        return -1;

    int32_t dr = (int32_t)raw_hi - (int32_t)raw_lo;
    int32_t ds = (int32_t)screen_hi - (int32_t)screen_lo;
    /* raw at pixel 0 and pixel (pixels-1), rounded to nearest. */
    int32_t r0 = (int32_t)raw_lo - (dr * (int32_t)screen_lo) / ds;
    int32_t redge =
        (int32_t)raw_lo + (dr * ((int32_t)pixels - 1 - (int32_t)screen_lo)) / ds;
    if (r0 < 0)
        r0 = 0;
    if (redge < 0)
        redge = 0;
    if (r0 > 0xFFFF)
        r0 = 0xFFFF;
    if (redge > 0xFFFF)
        redge = 0xFFFF;
    if (r0 == redge)
        return -1;

    *invert_out = r0 > redge;
    *min_out = (uint16_t)(r0 < redge ? r0 : redge);
    *max_out = (uint16_t)(r0 < redge ? redge : r0);
    return 0;
}

int touch_cal_solve(const touch_cal_point_t *points, unsigned count,
                    uint16_t screen_w, uint16_t screen_h,
                    stm32_touch_calibration_t *out) {
    if (!points || !out || count < 2 || screen_w < 2 || screen_h < 2)
        return -1;

    uint16_t sx_mid = (uint16_t)((screen_w - 1U) / 2U);
    uint16_t sy_mid = (uint16_t)((screen_h - 1U) / 2U);

    /*
     * Swap detection: see whether raw_x tracks the screen-X split or the
     * screen-Y split more strongly. If raw_x moves more with screen_y, the
     * panel's raw axes are swapped relative to the screen.
     */
    uint32_t rawx_dsx =
        abs_diff(side_mean(points, count, 1, 1, sx_mid, 1, 0, 0),
                 side_mean(points, count, 1, 1, sx_mid, 0, 0, 0));
    uint32_t rawx_dsy =
        abs_diff(side_mean(points, count, 1, 0, sy_mid, 1, 0, 0),
                 side_mean(points, count, 1, 0, sy_mid, 0, 0, 0));
    int swap = rawx_dsy > rawx_dsx;

    /* After swap, this raw axis feeds screen-X scaling, the other feeds Y. */
    int pick_for_x = swap ? 0 : 1; /* 1 = raw_x, 0 = raw_y */
    int pick_for_y = swap ? 1 : 0;

    unsigned nlo = 0, nhi = 0;
    uint32_t sxlo = 0, sxhi = 0, sylo = 0, syhi = 0;
    uint32_t rawx_lo =
        side_mean(points, count, pick_for_x, 1, sx_mid, 0, &sxlo, &nlo);
    uint32_t rawx_hi =
        side_mean(points, count, pick_for_x, 1, sx_mid, 1, &sxhi, &nhi);
    if (!nlo || !nhi)
        return -1;

    uint32_t rawy_lo =
        side_mean(points, count, pick_for_y, 0, sy_mid, 0, &sylo, &nlo);
    uint32_t rawy_hi =
        side_mean(points, count, pick_for_y, 0, sy_mid, 1, &syhi, &nhi);
    if (!nlo || !nhi)
        return -1;

    stm32_touch_calibration_t c;
    c.swap_xy = swap;

    if (fit_axis(rawx_lo, sxlo, rawx_hi, sxhi, screen_w, &c.x_min, &c.x_max,
                 &c.invert_x) != 0)
        return -1;
    if (fit_axis(rawy_lo, sylo, rawy_hi, syhi, screen_h, &c.y_min, &c.y_max,
                 &c.invert_y) != 0)
        return -1;

    *out = c;
    return 0;
}

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/*
 * Blob layout (little-endian, 20 bytes):
 *   [0]  magic0=0xCA  [1] magic1=0x1B  [2] version=1  [3] flags
 *   [4..5] x_min [6..7] x_max [8..9] y_min [10..11] y_max
 *   [12..17] reserved(0)  [18..19] crc16 over bytes [0..17]
 * flags bit0=swap_xy bit1=invert_x bit2=invert_y.
 */
int touch_cal_serialize(const stm32_touch_calibration_t *cal, uint8_t *buf,
                        unsigned buf_size) {
    if (!cal || !buf || buf_size < TOUCH_CAL_BLOB_SIZE)
        return -1;

    for (unsigned i = 0; i < TOUCH_CAL_BLOB_SIZE; i++)
        buf[i] = 0;
    buf[0] = TOUCH_CAL_MAGIC0;
    buf[1] = TOUCH_CAL_MAGIC1;
    buf[2] = TOUCH_CAL_VERSION;
    buf[3] = (uint8_t)((cal->swap_xy ? 1u : 0u) |
                       (cal->invert_x ? 2u : 0u) | (cal->invert_y ? 4u : 0u));
    put_u16(&buf[4], cal->x_min);
    put_u16(&buf[6], cal->x_max);
    put_u16(&buf[8], cal->y_min);
    put_u16(&buf[10], cal->y_max);
    put_u16(&buf[18], touch_crc16(buf, 18));
    return (int)TOUCH_CAL_BLOB_SIZE;
}

int touch_cal_deserialize(const uint8_t *buf, unsigned buf_size,
                          stm32_touch_calibration_t *cal) {
    if (!buf || !cal || buf_size < TOUCH_CAL_BLOB_SIZE)
        return -1;
    if (buf[0] != TOUCH_CAL_MAGIC0 || buf[1] != TOUCH_CAL_MAGIC1 ||
        buf[2] != TOUCH_CAL_VERSION)
        return -1;
    if (get_u16(&buf[18]) != touch_crc16(buf, 18))
        return -1;

    stm32_touch_calibration_t c;
    c.swap_xy = (buf[3] & 1u) ? 1 : 0;
    c.invert_x = (buf[3] & 2u) ? 1 : 0;
    c.invert_y = (buf[3] & 4u) ? 1 : 0;
    c.x_min = get_u16(&buf[4]);
    c.x_max = get_u16(&buf[6]);
    c.y_min = get_u16(&buf[8]);
    c.y_max = get_u16(&buf[10]);
    if (c.x_min >= c.x_max || c.y_min >= c.y_max)
        return -1;
    *cal = c;
    return 0;
}

#endif /* CONFIG_BOARD_STM32F103 */
