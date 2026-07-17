#ifdef CONFIG_BOARD_STM32F103

#include "display.h"
#include "backlight.h"
#include "core/string.h"

#define RCC_AHBENR  (*(volatile uint32_t *)0x40021014UL)
#define RCC_APB2ENR (*(volatile uint32_t *)0x40021018UL)

#define GPIOB_CRL (*(volatile uint32_t *)0x40010C00UL)
#define GPIOB_CRH (*(volatile uint32_t *)0x40010C04UL)
#define GPIOD_CRL (*(volatile uint32_t *)0x40011400UL)
#define GPIOD_CRH (*(volatile uint32_t *)0x40011404UL)
#define GPIOE_CRL (*(volatile uint32_t *)0x40011800UL)
#define GPIOE_CRH (*(volatile uint32_t *)0x40011804UL)
#define GPIOG_CRL (*(volatile uint32_t *)0x40012000UL)
#define GPIOG_CRH (*(volatile uint32_t *)0x40012004UL)
#define GPIOB_BSRR (*(volatile uint32_t *)0x40010C10UL)

#define FSMC_BCR4  (*(volatile uint32_t *)0xA0000018UL)
#define FSMC_BTR4  (*(volatile uint32_t *)0xA000001CUL)
#define FSMC_BWTR4 (*(volatile uint32_t *)0xA000011CUL)

#define LCD_CMD  (*(volatile uint16_t *)0x6C0007FEUL)
#define LCD_DATA (*(volatile uint16_t *)0x6C000800UL)

#define LCD_WIDTH  320U
#define LCD_HEIGHT 480U
#define LCD_BACKLIGHT_PIN 0U
#define UI_HEADER_HEIGHT 84U
#define UI_NAV_TOP       416U
#define UI_TOUCH_X       14U
#define UI_TOUCH_Y       148U
#define UI_TOUCH_W       292U
#define UI_TOUCH_H       196U
#define UI_CURSOR_STEP   12U

#define LCD_ID_HX8357D 0x0057U
#define LCD_ID_ILI9325 0x9325U
#define LCD_ID_ILI9328 0x9328U
#define LCD_ID_ILI9341 0x9341U
#define LCD_ID_ILI9481 0x9481U

#define RGB565(r, g, b) \
    (uint16_t)((((r) & 0xF8U) << 8) | (((g) & 0xFCU) << 3) | ((b) >> 3))

#define COLOR_NAVY   RGB565(12, 35, 64)
#define COLOR_BLUE   RGB565(21, 96, 189)
#define COLOR_CYAN   RGB565(35, 202, 222)
#define COLOR_GREEN  RGB565(44, 190, 112)
#define COLOR_RED    RGB565(230, 52, 62)
#define COLOR_YELLOW RGB565(246, 196, 57)
#define COLOR_WHITE  RGB565(245, 248, 252)
#define COLOR_MUTED  RGB565(164, 184, 205)
#define COLOR_DARK   RGB565(6, 17, 30)
#define COLOR_PANEL  RGB565(16, 32, 50)

/* ---- Legacy dashboard (kept as fallback; superseded by ui_home/ui_render path) ---- */
static uint64_t display_last_second = (uint64_t)-1;
static uint16_t display_controller_id;
static int display_ready;
static int display_sd_ready;
static int display_sd_fat32;
static int display_sd_bus_width;
static int display_touch_ready;
static int display_keys_ready;
static int display_light_ready;
static int display_auto_brightness;
static int display_bluetooth_ready;
static int display_bluetooth_detected;
static int display_bluetooth_at_responsive;
static int display_bluetooth_connected;
static int display_bluetooth_waiting;
static int display_bluetooth_configured;
static int display_bluetooth_slave;
static int display_bluetooth_uuid_supported;
static int display_bluetooth_uuid_configured;
static int display_wifi_active;
static int display_wifi_detected;
static int display_wifi_at_responsive;
static int display_wifi_configured;
static int display_wifi_connecting;
static int display_wifi_joined;
static int display_wifi_got_ip;
static int display_wifi_socket_connected;
static uint64_t display_sd_sectors;
static uint16_t display_bluetooth_uuid;
static uint32_t display_bluetooth_baud;
static uint32_t display_bluetooth_rx_bytes;
static uint32_t display_bluetooth_tx_bytes;
static uint32_t display_bluetooth_dropped;
static uint32_t display_wifi_access_points;
static uint32_t display_wifi_baud;
static uint32_t display_wifi_rx_bytes;
static uint32_t display_wifi_tx_bytes;
static uint32_t display_wifi_dropped;
static uint16_t display_light_raw;
static uint8_t display_light_percent;
static uint8_t display_backlight_percent = 100U;
static stm32_memory_info_t display_memory;
static char display_sd_label[12];
static char display_bluetooth_name[15];
static char display_bluetooth_pin[5];
static char display_bluetooth_line[15];
static char display_wifi_ssid[18];
static char display_wifi_ip[16];
static char display_wifi_mac[18];
static char display_wifi_scan_ssid[21];
static char display_wifi_event[25];
static int display_touch_pressed;
static uint16_t display_touch_x;
static uint16_t display_touch_y;
static uint16_t display_draw_x;
static uint16_t display_draw_y;
static int display_draw_valid;
static unsigned display_page;
static unsigned display_status_selection;
static stm32_display_action_t display_pending_action;
static uint16_t display_key_cursor_x = LCD_WIDTH / 2U;
static uint16_t display_key_cursor_y = UI_TOUCH_Y + UI_TOUCH_H / 2U;

enum {
    DISPLAY_PAGE_STATUS,
    DISPLAY_PAGE_MEMORY,
    DISPLAY_PAGE_STORAGE,
    DISPLAY_PAGE_BLUETOOTH,
    DISPLAY_PAGE_WIFI,
    DISPLAY_PAGE_TOUCH,
    DISPLAY_PAGE_COUNT,
};

static void delay_cycles(uint32_t cycles) {
    while (cycles--)
        __asm__ __volatile__("nop");
}

static void delay_ms(uint32_t ms) {
    while (ms--)
        delay_cycles(1600U);
}

static void gpio_config_pin(volatile uint32_t *crl, volatile uint32_t *crh,
                            unsigned pin, uint32_t mode) {
    volatile uint32_t *reg = pin < 8U ? crl : crh;
    uint32_t shift = (pin & 7U) * 4U;
    uint32_t value = *reg;
    value &= ~(0xFU << shift);
    value |= mode << shift;
    *reg = value;
}

static void lcd_write_cmd(uint16_t value) {
    LCD_CMD = value;
}

static void lcd_write_data(uint16_t value) {
    LCD_DATA = value;
}

static void lcd_write_color(uint16_t color) {
    if (display_controller_id == LCD_ID_HX8357D) {
        LCD_DATA = color >> 8;
        LCD_DATA = color & 0xFFU;
    } else {
        LCD_DATA = color;
    }
}

static uint16_t lcd_read_data(void) {
    return LCD_DATA;
}

static uint16_t lcd_read_id_dcs(void) {
    uint16_t id;

    lcd_write_cmd(0xD3);
    (void)lcd_read_data();
    (void)lcd_read_data();
    id = (uint16_t)(lcd_read_data() << 8);
    id |= lcd_read_data() & 0xFFU;
    return id;
}

static uint16_t lcd_read_id_hx8357d(void) {
    lcd_write_cmd(0xD0);
    (void)lcd_read_data();
    return lcd_read_data() & 0xFFU;
}

static void lcd_set_window_dcs(uint16_t x0, uint16_t y0,
                               uint16_t x1, uint16_t y1) {
    lcd_write_cmd(0x2A);
    lcd_write_data(x0 >> 8);
    lcd_write_data(x0 & 0xFFU);
    lcd_write_data(x1 >> 8);
    lcd_write_data(x1 & 0xFFU);

    lcd_write_cmd(0x2B);
    lcd_write_data(y0 >> 8);
    lcd_write_data(y0 & 0xFFU);
    lcd_write_data(y1 >> 8);
    lcd_write_data(y1 & 0xFFU);
    lcd_write_cmd(0x2C);
}

static void lcd_set_window_9325(uint16_t x0, uint16_t y0,
                                uint16_t x1, uint16_t y1) {
    lcd_write_cmd(0x50); lcd_write_data(x0);
    lcd_write_cmd(0x51); lcd_write_data(x1);
    lcd_write_cmd(0x52); lcd_write_data(y0);
    lcd_write_cmd(0x53); lcd_write_data(y1);
    lcd_write_cmd(0x20); lcd_write_data(x0);
    lcd_write_cmd(0x21); lcd_write_data(y0);
    lcd_write_cmd(0x22);
}

static void lcd_set_window(uint16_t x0, uint16_t y0,
                           uint16_t x1, uint16_t y1) {
    if (display_controller_id == LCD_ID_ILI9325 ||
        display_controller_id == LCD_ID_ILI9328)
        lcd_set_window_9325(x0, y0, x1, y1);
    else
        lcd_set_window_dcs(x0, y0, x1, y1);
}

static void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           uint16_t color) {
    if (!w || !h || x >= LCD_WIDTH || y >= LCD_HEIGHT)
        return;
    if ((uint32_t)x + w > LCD_WIDTH)
        w = LCD_WIDTH - x;
    if ((uint32_t)y + h > LCD_HEIGHT)
        h = LCD_HEIGHT - y;
    lcd_set_window(x, y, x + w - 1U, y + h - 1U);
    uint32_t pixels = (uint32_t)w * h;
    while (pixels--)
        lcd_write_color(color);
}

static void lcd_blit_stride(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                            const uint16_t *px, uint16_t source_stride) {
    unsigned row;

    if (!px || !w || !h || source_stride < w || x >= LCD_WIDTH ||
        y >= LCD_HEIGHT)
        return;
    if ((uint32_t)x + w > LCD_WIDTH)
        w = LCD_WIDTH - x;
    if ((uint32_t)y + h > LCD_HEIGHT)
        h = LCD_HEIGHT - y;

    lcd_set_window(x, y, x + w - 1U, y + h - 1U);
    for (row = 0; row < h; row++) {
        uint16_t column;
        for (column = 0; column < w; column++)
            lcd_write_color(*px++);
        px += source_stride - w;
    }
}

void stm32_display_blit(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                        const uint16_t *px) {
    lcd_blit_stride(x, y, w, h, px, w);
}

void stm32_display_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                             uint16_t color) {
    if (!w || !h || x >= LCD_WIDTH || y >= LCD_HEIGHT)
        return;
    if ((uint32_t)x + w > LCD_WIDTH)
        w = LCD_WIDTH - x;
    if ((uint32_t)y + h > LCD_HEIGHT)
        h = LCD_HEIGHT - y;

    lcd_fill_rect(x, y, w, h, color);
}

static void stm32_display_gfx_fill_rect(void *ctx, int x, int y, int w,
                                         int h, uint16_t color) {
    (void)ctx;
    if (w <= 0 || h <= 0 || x >= (int)LCD_WIDTH || y >= (int)LCD_HEIGHT)
        return;
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (w <= 0 || h <= 0)
        return;
    stm32_display_fill_rect((uint16_t)x, (uint16_t)y, (uint16_t)w,
                            (uint16_t)h, color);
}

static void stm32_display_gfx_blit(void *ctx, int x, int y, int w, int h,
                                    const uint16_t *px) {
    int source_w = w;

    (void)ctx;
    if (!px || w <= 0 || h <= 0 || x >= (int)LCD_WIDTH ||
        y >= (int)LCD_HEIGHT)
        return;
    if (x < 0) {
        if (w <= -x)
            return;
        px += -x;
        w += x;
        x = 0;
    }
    if (y < 0) {
        if (h <= -y)
            return;
        px += (size_t)(-y) * source_w;
        h += y;
        y = 0;
    }
    lcd_blit_stride((uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h, px,
                    (uint16_t)source_w);
}

void stm32_display_gfx(ui_gfx_t *gfx) {
    if (!gfx)
        return;
    gfx->ctx = NULL;
    gfx->fill_rect = stm32_display_gfx_fill_rect;
    gfx->blit = stm32_display_gfx_blit;
}

static const uint8_t font5x7[][5] = {
    [' ' - 32] = {0x00, 0x00, 0x00, 0x00, 0x00},
    ['-' - 32] = {0x08, 0x08, 0x08, 0x08, 0x08},
    ['.' - 32] = {0x00, 0x60, 0x60, 0x00, 0x00},
    ['/' - 32] = {0x60, 0x18, 0x06, 0x01, 0x00},
    ['0' - 32] = {0x3E, 0x51, 0x49, 0x45, 0x3E},
    ['1' - 32] = {0x00, 0x42, 0x7F, 0x40, 0x00},
    ['2' - 32] = {0x62, 0x51, 0x49, 0x49, 0x46},
    ['3' - 32] = {0x22, 0x49, 0x49, 0x49, 0x36},
    ['4' - 32] = {0x18, 0x14, 0x12, 0x7F, 0x10},
    ['5' - 32] = {0x2F, 0x49, 0x49, 0x49, 0x31},
    ['6' - 32] = {0x3E, 0x49, 0x49, 0x49, 0x32},
    ['7' - 32] = {0x01, 0x71, 0x09, 0x05, 0x03},
    ['8' - 32] = {0x36, 0x49, 0x49, 0x49, 0x36},
    ['9' - 32] = {0x26, 0x49, 0x49, 0x49, 0x3E},
    [':' - 32] = {0x00, 0x36, 0x36, 0x00, 0x00},
    ['A' - 32] = {0x7E, 0x11, 0x11, 0x11, 0x7E},
    ['B' - 32] = {0x7F, 0x49, 0x49, 0x49, 0x36},
    ['C' - 32] = {0x3E, 0x41, 0x41, 0x41, 0x22},
    ['D' - 32] = {0x7F, 0x41, 0x41, 0x22, 0x1C},
    ['E' - 32] = {0x7F, 0x49, 0x49, 0x49, 0x41},
    ['F' - 32] = {0x7F, 0x09, 0x09, 0x09, 0x01},
    ['G' - 32] = {0x3E, 0x41, 0x49, 0x49, 0x7A},
    ['H' - 32] = {0x7F, 0x08, 0x08, 0x08, 0x7F},
    ['I' - 32] = {0x00, 0x41, 0x7F, 0x41, 0x00},
    ['J' - 32] = {0x20, 0x40, 0x41, 0x3F, 0x01},
    ['K' - 32] = {0x7F, 0x08, 0x14, 0x22, 0x41},
    ['L' - 32] = {0x7F, 0x40, 0x40, 0x40, 0x40},
    ['M' - 32] = {0x7F, 0x02, 0x0C, 0x02, 0x7F},
    ['N' - 32] = {0x7F, 0x04, 0x08, 0x10, 0x7F},
    ['O' - 32] = {0x3E, 0x41, 0x41, 0x41, 0x3E},
    ['P' - 32] = {0x7F, 0x09, 0x09, 0x09, 0x06},
    ['Q' - 32] = {0x3E, 0x41, 0x51, 0x21, 0x5E},
    ['R' - 32] = {0x7F, 0x09, 0x19, 0x29, 0x46},
    ['S' - 32] = {0x46, 0x49, 0x49, 0x49, 0x31},
    ['T' - 32] = {0x01, 0x01, 0x7F, 0x01, 0x01},
    ['U' - 32] = {0x3F, 0x40, 0x40, 0x40, 0x3F},
    ['V' - 32] = {0x1F, 0x20, 0x40, 0x20, 0x1F},
    ['W' - 32] = {0x7F, 0x20, 0x18, 0x20, 0x7F},
    ['X' - 32] = {0x63, 0x14, 0x08, 0x14, 0x63},
    ['Y' - 32] = {0x03, 0x04, 0x78, 0x04, 0x03},
    ['Z' - 32] = {0x61, 0x51, 0x49, 0x45, 0x43},
};

static void lcd_draw_char(uint16_t x, uint16_t y, char ch, uint16_t color,
                          uint16_t bg, unsigned scale) {
    if (ch < 32 || ch > 'Z')
        ch = ' ';
    const uint8_t *glyph = font5x7[(unsigned)ch - 32U];
    for (unsigned col = 0; col < 5; col++) {
        for (unsigned row = 0; row < 7; row++) {
            uint16_t c = (glyph[col] & (1U << row)) ? color : bg;
            lcd_fill_rect(x + col * scale, y + row * scale, scale, scale, c);
        }
    }
    lcd_fill_rect(x + 5U * scale, y, scale, 7U * scale, bg);
}

static void lcd_draw_text(uint16_t x, uint16_t y, const char *text,
                          uint16_t color, uint16_t bg, unsigned scale) {
    while (*text) {
        char ch = *text++;
        if (ch >= 'a' && ch <= 'z')
            ch -= 'a' - 'A';
        lcd_draw_char(x, y, ch, color, bg, scale);
        x += 6U * scale;
    }
}

static void lcd_draw_decimal(uint16_t x, uint16_t y, uint64_t value,
                             uint16_t color, uint16_t bg, unsigned scale) {
    char buf[21];
    unsigned pos = sizeof(buf);
    buf[--pos] = '\0';
    do {
        buf[--pos] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value && pos);
    lcd_draw_text(x, y, &buf[pos], color, bg, scale);
}

static void lcd_draw_hex4(uint16_t x, uint16_t y, uint16_t value,
                          uint16_t color, uint16_t bg, unsigned scale) {
    char text[5];

    for (unsigned i = 0; i < 4U; i++) {
        unsigned shift = (3U - i) * 4U;
        unsigned digit = (value >> shift) & 0xFU;
        text[i] = digit < 10U ? (char)('0' + digit)
                              : (char)('A' + digit - 10U);
    }
    text[4] = '\0';
    lcd_draw_text(x, y, text, color, bg, scale);
}

static uint16_t lcd_text_width(const char *text, unsigned scale) {
    uint16_t width = 0;
    while (*text++) {
        if (width > LCD_WIDTH - 6U * scale)
            return LCD_WIDTH;
        width += 6U * scale;
    }
    return width;
}

static void lcd_draw_border(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                            uint16_t color, uint16_t inside) {
    if (w < 4U || h < 4U)
        return;
    lcd_fill_rect(x, y, w, h, color);
    lcd_fill_rect(x + 2U, y + 2U, w - 4U, h - 4U, inside);
}

static void lcd_draw_button(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                            const char *label, int active) {
    uint16_t border = active ? COLOR_CYAN : COLOR_MUTED;
    uint16_t fill = active ? COLOR_BLUE : COLOR_PANEL;
    uint16_t text_width = lcd_text_width(label, 2);
    lcd_draw_border(x, y, w, h, border, fill);
    lcd_draw_text(x + (w - text_width) / 2U, y + (h - 14U) / 2U,
                  label, COLOR_WHITE, fill, 2);
}

static void lcd_draw_status_row(uint16_t y, const char *label,
                                const char *state, uint16_t color,
                                int selected) {
    uint16_t border = selected ? COLOR_CYAN : COLOR_PANEL;
    lcd_draw_border(14, y, 292, 30, border, COLOR_PANEL);
    lcd_fill_rect(16, y + 2U, 5, 26, color);
    lcd_draw_text(28, y + 8U, label, COLOR_WHITE, COLOR_PANEL, 2);
    lcd_draw_text(206, y + 8U, state, color, COLOR_PANEL, 2);
}

static void lcd_draw_light_values(int clear) {
    if (clear) {
        lcd_fill_rect(106, 350, 94, 24, COLOR_PANEL);
        lcd_fill_rect(246, 350, 56, 14, COLOR_PANEL);
    }
    lcd_draw_decimal(112, 356, display_light_percent,
                     COLOR_WHITE, COLOR_PANEL, 2);
    lcd_draw_text(160, 356, "PCT", COLOR_WHITE, COLOR_PANEL, 2);
    lcd_draw_decimal(250, 354, display_light_raw,
                     COLOR_WHITE, COLOR_PANEL, 1);
}

static void lcd_draw_light_panel(void) {
    lcd_draw_border(14, 316, 292, 84, COLOR_BLUE, COLOR_PANEL);
    lcd_draw_text(28, 328, "AMBIENT LIGHT", COLOR_MUTED, COLOR_PANEL, 2);
    lcd_draw_text(28, 356, "LEVEL", COLOR_CYAN, COLOR_PANEL, 2);
    if (!display_light_ready) {
        lcd_draw_text(112, 356, "SENSOR ABSENT",
                      COLOR_YELLOW, COLOR_PANEL, 2);
        return;
    }

    lcd_draw_text(226, 354, "ADC", COLOR_MUTED, COLOR_PANEL, 1);
    lcd_draw_text(28, 382, "BACKLIGHT FIXED ON",
                  COLOR_MUTED, COLOR_PANEL, 1);
    lcd_draw_light_values(0);
}

static void lcd_draw_header(void) {
    lcd_fill_rect(0, 0, LCD_WIDTH, UI_HEADER_HEIGHT, COLOR_NAVY);
    lcd_fill_rect(0, UI_HEADER_HEIGHT - 4U, LCD_WIDTH, 4, COLOR_CYAN);
    lcd_draw_text(16, 14, "A20OS", COLOR_WHITE, COLOR_NAVY, 5);
    lcd_draw_text(196, 16, "STM32F103", COLOR_MUTED, COLOR_NAVY, 2);
    lcd_draw_text(196, 48, "UP", COLOR_CYAN, COLOR_NAVY, 2);
}

static void lcd_draw_navigation(void) {
    static const char *const labels[DISPLAY_PAGE_COUNT] = {
        "SYS", "MEM", "TF", "BT", "WIFI", "IN",
    };

    lcd_fill_rect(0, UI_NAV_TOP, LCD_WIDTH,
                  LCD_HEIGHT - UI_NAV_TOP, COLOR_DARK);
    for (unsigned i = 0; i < DISPLAY_PAGE_COUNT; i++)
        lcd_draw_button(i * 53U, UI_NAV_TOP + 7U,
                        i + 1U == DISPLAY_PAGE_COUNT ? 55U : 53U, 50U,
                        labels[i], display_page == i);
}

static void lcd_draw_status_page(void) {
    lcd_fill_rect(0, UI_HEADER_HEIGHT, LCD_WIDTH,
                  UI_NAV_TOP - UI_HEADER_HEIGHT, COLOR_DARK);
    lcd_draw_text(16, 98, "SYSTEM STATUS", COLOR_CYAN, COLOR_DARK, 3);
    lcd_draw_status_row(128, "MEMORY", "LIVE",
                        COLOR_GREEN,
                        display_status_selection == 0U);
    lcd_draw_status_row(158, "TF CARD",
                        display_sd_ready ? "READY" : "OPTIONAL",
                        display_sd_ready ? COLOR_GREEN : COLOR_YELLOW,
                        display_status_selection == 1U);
    lcd_draw_status_row(188, "BLUETOOTH",
                        display_bluetooth_connected ? "LINKED" :
                        display_bluetooth_waiting ? "WAITING" :
                        display_bluetooth_ready ? "CONFIG" : "OPTIONAL",
                        display_bluetooth_connected ? COLOR_GREEN :
                        display_bluetooth_waiting ? COLOR_CYAN :
                                                   COLOR_YELLOW,
                        display_status_selection == 2U);
    lcd_draw_status_row(218, "TOUCH",
                        display_touch_ready ? "ARMED" : "OPTIONAL",
                        display_touch_ready ? COLOR_CYAN : COLOR_YELLOW,
                        display_status_selection == 3U);
    lcd_draw_status_row(248, "LIGHT",
                        display_light_ready ?
                            (display_auto_brightness ? "AUTO" : "READY") :
                            "OPTIONAL",
                        display_light_ready ? COLOR_CYAN : COLOR_YELLOW,
                        display_status_selection == 4U);
    lcd_draw_status_row(278, "DIR KEYS",
                        display_keys_ready ? "READY" : "OPTIONAL",
                        display_keys_ready ? COLOR_GREEN : COLOR_YELLOW,
                        display_status_selection == 5U);

    lcd_draw_light_panel();
}

static void lcd_draw_usage_line(uint16_t y, const char *label,
                                size_t used, size_t total,
                                uint16_t color) {
    lcd_draw_text(28, y, label, COLOR_MUTED, COLOR_PANEL, 2);
    lcd_draw_decimal(116, y + 4U, used, color, COLOR_PANEL, 1);
    lcd_draw_text(164, y + 4U, "/", COLOR_MUTED, COLOR_PANEL, 1);
    lcd_draw_decimal(176, y + 4U, total, COLOR_WHITE, COLOR_PANEL, 1);
    lcd_draw_text(226, y + 4U, "BYTES", COLOR_MUTED, COLOR_PANEL, 1);
}

static void lcd_draw_memory_page(void) {
    lcd_fill_rect(0, UI_HEADER_HEIGHT, LCD_WIDTH,
                  UI_NAV_TOP - UI_HEADER_HEIGHT, COLOR_DARK);
    lcd_draw_text(16, 98, "MEMORY", COLOR_CYAN, COLOR_DARK, 3);

    lcd_draw_border(14, 132, 292, 112, COLOR_GREEN, COLOR_PANEL);
    lcd_draw_text(28, 146,
                  display_memory.ram_capacity_from_silicon ?
                      "MCU SRAM HW" : "MCU SRAM LINK",
                  COLOR_GREEN, COLOR_PANEL, 2);
    lcd_draw_usage_line(176, "USED", display_memory.internal_ram_used,
                        display_memory.internal_ram_total, COLOR_WHITE);
    lcd_draw_usage_line(204, "HEAP",
                        display_memory.internal_heap_used,
                        display_memory.internal_heap_total, COLOR_WHITE);
    lcd_draw_text(28, 228, "STACK PEAK", COLOR_MUTED, COLOR_PANEL, 2);
    lcd_draw_decimal(172, 232, display_memory.stack_peak_bytes,
                     COLOR_WHITE, COLOR_PANEL, 1);
    lcd_draw_text(220, 232, "BYTES", COLOR_MUTED, COLOR_PANEL, 1);

    lcd_draw_border(14, 254, 292, 66,
                    display_memory.external_ram_total ?
                        COLOR_CYAN : COLOR_YELLOW,
                    COLOR_PANEL);
    lcd_draw_text(28, 266, "EXT SRAM",
                  display_memory.external_ram_total ?
                      COLOR_CYAN : COLOR_YELLOW,
                  COLOR_PANEL, 2);
    if (display_memory.external_ram_total) {
        lcd_draw_usage_line(294, "USED",
                            display_memory.external_ram_used,
                            display_memory.external_ram_total, COLOR_WHITE);
    } else {
        lcd_draw_text(184, 294, "ABSENT", COLOR_YELLOW, COLOR_PANEL, 2);
    }

    lcd_draw_border(14, 330, 292, 70, COLOR_BLUE, COLOR_PANEL);
    lcd_draw_text(28, 342,
                  display_memory.flash_capacity_from_silicon ?
                      "FLASH HW" : "FLASH LINK",
                  COLOR_CYAN, COLOR_PANEL, 2);
    lcd_draw_usage_line(372, "USED", display_memory.flash_used,
                        display_memory.flash_total, COLOR_WHITE);
}

static void lcd_draw_bluetooth_page(void) {
    lcd_fill_rect(0, UI_HEADER_HEIGHT, LCD_WIDTH,
                  UI_NAV_TOP - UI_HEADER_HEIGHT, COLOR_DARK);
    lcd_draw_text(16, 98, "BLUETOOTH", COLOR_CYAN, COLOR_DARK, 3);

    if (!display_bluetooth_ready) {
        lcd_draw_border(14, 148, 292, 190, COLOR_YELLOW, COLOR_PANEL);
        lcd_draw_text(52, 196, "HC-05 DISABLED", COLOR_YELLOW,
                      COLOR_PANEL, 3);
        return;
    }

    lcd_draw_border(14, 138, 292, 264,
                    display_bluetooth_connected ? COLOR_GREEN : COLOR_CYAN,
                    COLOR_PANEL);
    lcd_draw_text(28, 154,
                  display_bluetooth_connected ? "LINK CONNECTED" :
                  display_bluetooth_waiting ? "SLAVE WAITING" :
                  display_bluetooth_at_responsive ? "CONFIG UNVERIFIED" :
                  display_bluetooth_detected ? "MODULE DETECTED" :
                                               "MODULE UNKNOWN",
                  display_bluetooth_connected ? COLOR_GREEN :
                  display_bluetooth_waiting ? COLOR_CYAN : COLOR_YELLOW,
                  COLOR_PANEL, 2);

    lcd_draw_text(28, 188, "NAME", COLOR_MUTED, COLOR_PANEL, 2);
    lcd_draw_text(112, 188, display_bluetooth_name,
                  COLOR_WHITE, COLOR_PANEL, 2);
    lcd_draw_text(28, 218, "PIN", COLOR_MUTED, COLOR_PANEL, 2);
    lcd_draw_text(112, 218, display_bluetooth_pin,
                  COLOR_WHITE, COLOR_PANEL, 2);

    lcd_draw_text(28, 248, "UUID", COLOR_MUTED, COLOR_PANEL, 2);
    lcd_draw_text(112, 248, "0X", COLOR_WHITE, COLOR_PANEL, 2);
    lcd_draw_hex4(136, 248, display_bluetooth_uuid,
                  COLOR_WHITE, COLOR_PANEL, 2);
    lcd_draw_text(208, 248,
                  display_bluetooth_uuid_configured ? "SET" :
                  display_bluetooth_uuid_supported ? "FAIL" : "SPP",
                  display_bluetooth_uuid_configured ?
                      COLOR_GREEN :
                  display_bluetooth_uuid_supported ?
                      COLOR_YELLOW : COLOR_CYAN,
                  COLOR_PANEL, 2);

    lcd_draw_text(28, 278, "ROLE", COLOR_MUTED, COLOR_PANEL, 2);
    lcd_draw_text(112, 278,
                  display_bluetooth_slave ? "SLAVE" : "UNKNOWN",
                  display_bluetooth_slave ? COLOR_GREEN : COLOR_YELLOW,
                  COLOR_PANEL, 2);
    lcd_draw_text(28, 308, "UART", COLOR_MUTED, COLOR_PANEL, 2);
    lcd_draw_decimal(112, 308, display_bluetooth_baud,
                     COLOR_WHITE, COLOR_PANEL, 2);
    lcd_draw_text(208, 308, "8N1", COLOR_WHITE, COLOR_PANEL, 2);

    lcd_draw_text(28, 338, "RX", COLOR_MUTED, COLOR_PANEL, 1);
    lcd_draw_decimal(52, 338, display_bluetooth_rx_bytes,
                     COLOR_WHITE, COLOR_PANEL, 1);
    lcd_draw_text(112, 338, "TX", COLOR_MUTED, COLOR_PANEL, 1);
    lcd_draw_decimal(136, 338, display_bluetooth_tx_bytes,
                     COLOR_WHITE, COLOR_PANEL, 1);
    lcd_draw_text(196, 338, "DROP", COLOR_MUTED, COLOR_PANEL, 1);
    lcd_draw_decimal(232, 338, display_bluetooth_dropped,
                     display_bluetooth_dropped ? COLOR_YELLOW : COLOR_WHITE,
                     COLOR_PANEL, 1);
    lcd_draw_button(72, 364, 176, 32, "SEND TEST", 0);
}

static void lcd_draw_wifi_page(void) {
    lcd_fill_rect(0, UI_HEADER_HEIGHT, LCD_WIDTH,
                  UI_NAV_TOP - UI_HEADER_HEIGHT, COLOR_DARK);
    lcd_draw_text(16, 98, "WIFI ESP8266", COLOR_CYAN, COLOR_DARK, 3);

    lcd_draw_border(14, 132, 292, 270,
                    display_wifi_got_ip ? COLOR_GREEN :
                    display_wifi_detected ? COLOR_CYAN : COLOR_YELLOW,
                    COLOR_PANEL);
    lcd_draw_text(28, 146,
                  display_wifi_got_ip ? "STA ONLINE" :
                  display_wifi_connecting ? "JOINING AP" :
                  display_wifi_at_responsive ? "AT READY" :
                  display_wifi_detected ? "MODULE DETECTED" :
                  display_wifi_active &&
                      strcmp(display_wifi_event, "MODULE NOT FOUND") != 0 ?
                                      "PROBING MODULE" :
                  display_wifi_active ? "MODULE NOT FOUND" :
                                        "SHARED SOCKET BUSY",
                  display_wifi_got_ip ? COLOR_GREEN :
                  display_wifi_detected ? COLOR_CYAN : COLOR_YELLOW,
                  COLOR_PANEL, 2);

    lcd_draw_text(28, 176, "SSID", COLOR_MUTED, COLOR_PANEL, 2);
    lcd_draw_text(100, 176,
                  display_wifi_ssid[0] ? display_wifi_ssid : "NOT SET",
                  COLOR_WHITE, COLOR_PANEL, 2);
    lcd_draw_text(28, 204, "IP", COLOR_MUTED, COLOR_PANEL, 2);
    lcd_draw_text(100, 204,
                  display_wifi_ip[0] ? display_wifi_ip : "0.0.0.0",
                  COLOR_WHITE, COLOR_PANEL, 2);
    lcd_draw_text(28, 232, "MAC", COLOR_MUTED, COLOR_PANEL, 2);
    lcd_draw_text(100, 232,
                  display_wifi_mac[0] ? display_wifi_mac : "UNKNOWN",
                  COLOR_WHITE, COLOR_PANEL, 1);
    lcd_draw_text(28, 260, "SOCKET", COLOR_MUTED, COLOR_PANEL, 2);
    lcd_draw_text(124, 260,
                  display_wifi_socket_connected ? "CONNECTED" : "IDLE",
                  display_wifi_socket_connected ? COLOR_GREEN : COLOR_MUTED,
                  COLOR_PANEL, 2);
    lcd_draw_text(28, 288, "APS", COLOR_MUTED, COLOR_PANEL, 2);
    lcd_draw_decimal(76, 288, display_wifi_access_points,
                     COLOR_WHITE, COLOR_PANEL, 2);
    lcd_draw_text(124, 288,
                  display_wifi_scan_ssid[0] ? display_wifi_scan_ssid : "NONE",
                  COLOR_WHITE, COLOR_PANEL, 1);
    lcd_draw_text(28, 314, display_wifi_event,
                  COLOR_CYAN, COLOR_PANEL, 1);
    lcd_draw_text(28, 338, "RX", COLOR_MUTED, COLOR_PANEL, 1);
    lcd_draw_decimal(52, 338, display_wifi_rx_bytes,
                     COLOR_WHITE, COLOR_PANEL, 1);
    lcd_draw_text(112, 338, "TX", COLOR_MUTED, COLOR_PANEL, 1);
    lcd_draw_decimal(136, 338, display_wifi_tx_bytes,
                     COLOR_WHITE, COLOR_PANEL, 1);
    lcd_draw_text(196, 338, "DROP", COLOR_MUTED, COLOR_PANEL, 1);
    lcd_draw_decimal(232, 338, display_wifi_dropped,
                     display_wifi_dropped ? COLOR_YELLOW : COLOR_WHITE,
                     COLOR_PANEL, 1);
    lcd_draw_button(72, 364, 176, 32, "SCAN AP", 0);
}

static void lcd_draw_storage_page(void) {
    lcd_fill_rect(0, UI_HEADER_HEIGHT, LCD_WIDTH,
                  UI_NAV_TOP - UI_HEADER_HEIGHT, COLOR_DARK);
    lcd_draw_text(16, 98, "TF STORAGE", COLOR_CYAN, COLOR_DARK, 3);
    if (!display_sd_ready) {
        lcd_draw_border(14, 148, 292, 190, COLOR_YELLOW, COLOR_PANEL);
        lcd_draw_text(82, 196, "NO TF CARD", COLOR_YELLOW,
                      COLOR_PANEL, 3);
        lcd_draw_text(70, 248, "SDIO WAITING", COLOR_MUTED,
                      COLOR_PANEL, 2);
        return;
    }

    lcd_draw_border(14, 138, 292, 252, COLOR_GREEN, COLOR_PANEL);
    lcd_draw_text(28, 154, "CARD ONLINE", COLOR_GREEN, COLOR_PANEL, 3);
    lcd_draw_text(28, 208, "SIZE", COLOR_MUTED, COLOR_PANEL, 2);
    lcd_draw_decimal(148, 208, display_sd_sectors / 2048U,
                     COLOR_WHITE, COLOR_PANEL, 2);
    lcd_draw_text(244, 208, "MB", COLOR_WHITE, COLOR_PANEL, 2);
    lcd_draw_text(28, 246, "FILESYS", COLOR_MUTED, COLOR_PANEL, 2);
    lcd_draw_text(148, 246, display_sd_fat32 ? "FAT32" : "RAW",
                  display_sd_fat32 ? COLOR_GREEN : COLOR_YELLOW,
                  COLOR_PANEL, 2);
    lcd_draw_text(28, 284, "SDIO BUS", COLOR_MUTED, COLOR_PANEL, 2);
    lcd_draw_decimal(148, 284, display_sd_bus_width,
                     COLOR_WHITE, COLOR_PANEL, 2);
    lcd_draw_text(172, 284, "BIT", COLOR_WHITE, COLOR_PANEL, 2);
    lcd_draw_text(28, 322, "LABEL", COLOR_MUTED, COLOR_PANEL, 2);
    lcd_draw_text(148, 322,
                  display_sd_label[0] ? display_sd_label : "NONE",
                  COLOR_WHITE, COLOR_PANEL, 2);
}

static void lcd_clear_touch_canvas(void) {
    lcd_draw_border(UI_TOUCH_X, UI_TOUCH_Y, UI_TOUCH_W, UI_TOUCH_H,
                    COLOR_BLUE, COLOR_PANEL);
    display_draw_valid = 0;
}

static void lcd_draw_touch_coordinates(void) {
    lcd_fill_rect(184, 120, 122, 26, COLOR_DARK);
    lcd_draw_text(184, 126, "X", COLOR_CYAN, COLOR_DARK, 2);
    lcd_draw_decimal(202, 126, display_touch_x,
                     COLOR_WHITE, COLOR_DARK, 2);
    lcd_draw_text(256, 126, "Y", COLOR_CYAN, COLOR_DARK, 2);
    lcd_draw_decimal(274, 126, display_touch_y,
                     COLOR_WHITE, COLOR_DARK, 2);
}

static void lcd_draw_key_cursor(void) {
    lcd_draw_border(display_key_cursor_x - 4U, display_key_cursor_y - 4U,
                    9, 9, COLOR_YELLOW, COLOR_DARK);
    lcd_fill_rect(display_key_cursor_x - 1U, display_key_cursor_y - 1U,
                  3, 3, COLOR_YELLOW);
}

static void lcd_draw_touch_page(void) {
    lcd_fill_rect(0, UI_HEADER_HEIGHT, LCD_WIDTH,
                  UI_NAV_TOP - UI_HEADER_HEIGHT, COLOR_DARK);
    lcd_draw_text(16, 98, "INPUT TEST", COLOR_CYAN, COLOR_DARK, 3);
    if (!display_touch_pressed) {
        display_touch_x = display_key_cursor_x;
        display_touch_y = display_key_cursor_y;
    }
    lcd_draw_touch_coordinates();
    lcd_clear_touch_canvas();
    lcd_draw_key_cursor();
    lcd_draw_button(92, 358, 136, 44, "CLEAR", 0);
}

static void lcd_render_page(void) {
    if (!display_ready)
        return;
    if (display_page == DISPLAY_PAGE_STORAGE)
        lcd_draw_storage_page();
    else if (display_page == DISPLAY_PAGE_MEMORY)
        lcd_draw_memory_page();
    else if (display_page == DISPLAY_PAGE_BLUETOOTH)
        lcd_draw_bluetooth_page();
    else if (display_page == DISPLAY_PAGE_WIFI)
        lcd_draw_wifi_page();
    else if (display_page == DISPLAY_PAGE_TOUCH)
        lcd_draw_touch_page();
    else
        lcd_draw_status_page();
    lcd_draw_navigation();
}

static void lcd_draw_brush(uint16_t x, uint16_t y) {
    if (x < UI_TOUCH_X + 3U || x >= UI_TOUCH_X + UI_TOUCH_W - 3U ||
        y < UI_TOUCH_Y + 3U || y >= UI_TOUCH_Y + UI_TOUCH_H - 3U)
        return;
    lcd_fill_rect(x - 2U, y - 2U, 5, 5, COLOR_CYAN);
}

static void lcd_draw_touch_line(uint16_t x0, uint16_t y0,
                                uint16_t x1, uint16_t y1) {
    int x = x0;
    int y = y0;
    int target_x = x1;
    int target_y = y1;
    int dx = target_x > x ? target_x - x : x - target_x;
    int sx = x < target_x ? 1 : -1;
    int dy_abs = target_y > y ? target_y - y : y - target_y;
    int dy = -dy_abs;
    int sy = y < target_y ? 1 : -1;
    int error = dx + dy;

    for (;;) {
        lcd_draw_brush((uint16_t)x, (uint16_t)y);
        if (x == target_x && y == target_y)
            break;
        int twice = error * 2;
        if (twice >= dy) {
            error += dy;
            x += sx;
        }
        if (twice <= dx) {
            error += dx;
            y += sy;
        }
    }
}

static void lcd_copy_label(const char *label) {
    unsigned i = 0;
    if (label) {
        while (i + 1U < sizeof(display_sd_label) && label[i]) {
            display_sd_label[i] = label[i];
            i++;
        }
    }
    display_sd_label[i] = '\0';
    while (++i < sizeof(display_sd_label))
        display_sd_label[i] = '\0';
}

static void lcd_copy_short_text(char *dest, size_t capacity,
                                const char *source) {
    unsigned i = 0;

    if (capacity == 0)
        return;
    if (source) {
        while (i + 1U < capacity && source[i]) {
            char c = source[i];
            dest[i] = c >= 32 && c <= 126 ? c : '.';
            i++;
        }
    }
    dest[i] = '\0';
    while (++i < capacity)
        dest[i] = '\0';
}

static void lcd_select_page(unsigned page) {
    if (page >= DISPLAY_PAGE_COUNT || page == display_page)
        return;
    display_page = page;
    display_draw_valid = 0;
    lcd_render_page();
}

static void lcd_select_relative_page(int direction) {
    unsigned page;

    if (direction < 0)
        page = display_page == 0U ? DISPLAY_PAGE_COUNT - 1U
                                 : display_page - 1U;
    else
        page = (display_page + 1U) % DISPLAY_PAGE_COUNT;
    lcd_select_page(page);
}

static void lcd_handle_touch_down(uint16_t x, uint16_t y, int new_press) {
    if (y >= UI_NAV_TOP) {
        if (!new_press)
            return;
        unsigned page = x / 53U;
        if (page >= DISPLAY_PAGE_COUNT)
            page = DISPLAY_PAGE_COUNT - 1U;
        lcd_select_page(page);
        return;
    }

    if (display_page == DISPLAY_PAGE_STATUS) {
        if (!new_press)
            return;
        if (y >= 124U && y < 154U) {
            lcd_select_page(DISPLAY_PAGE_MEMORY);
            return;
        }
        if (y >= 154U && y < 184U) {
            lcd_select_page(DISPLAY_PAGE_STORAGE);
            return;
        }
        if (y >= 184U && y < 214U) {
            lcd_select_page(DISPLAY_PAGE_BLUETOOTH);
            return;
        }
        if (y >= 214U && y < 244U) {
            lcd_select_page(DISPLAY_PAGE_TOUCH);
            return;
        }
    }

    if (display_page == DISPLAY_PAGE_BLUETOOTH) {
        if (new_press && y >= 358U && y < 406U) {
            display_pending_action =
                STM32_DISPLAY_ACTION_BLUETOOTH_TEST;
            lcd_draw_button(72, 364, 176, 32, "SEND TEST", 1);
        }
        return;
    }

    if (display_page == DISPLAY_PAGE_WIFI) {
        if (new_press && y >= 358U && y < 406U) {
            display_pending_action = STM32_DISPLAY_ACTION_WIFI_SCAN;
            lcd_draw_button(72, 364, 176, 32, "SCAN AP", 1);
        }
        return;
    }

    if (display_page != DISPLAY_PAGE_TOUCH)
        return;
    if (y >= 352U && y < 408U) {
        if (!new_press)
            return;
        lcd_clear_touch_canvas();
        lcd_draw_button(92, 358, 136, 44, "CLEAR", 1);
        return;
    }
    if (x <= UI_TOUCH_X + 2U || x >= UI_TOUCH_X + UI_TOUCH_W - 2U ||
        y <= UI_TOUCH_Y + 2U || y >= UI_TOUCH_Y + UI_TOUCH_H - 2U)
        return;

    if (display_draw_valid && x == display_draw_x && y == display_draw_y)
        return;
    if (display_draw_valid)
        lcd_draw_touch_line(display_draw_x, display_draw_y, x, y);
    else
        lcd_draw_brush(x, y);
    display_draw_x = x;
    display_draw_y = y;
    display_draw_valid = 1;
}

static void lcd_redraw_peripheral_status(void) {
    if (!display_ready)
        return;
    if (display_page == DISPLAY_PAGE_STATUS ||
        display_page == DISPLAY_PAGE_MEMORY ||
        display_page == DISPLAY_PAGE_STORAGE ||
        display_page == DISPLAY_PAGE_BLUETOOTH ||
        display_page == DISPLAY_PAGE_WIFI)
        lcd_render_page();
}

static void lcd_redraw_touch_status(void) {
    if (display_page == DISPLAY_PAGE_TOUCH) {
        lcd_draw_touch_coordinates();
    }
}

static void lcd_redraw_touch_released(void) {
    (void)0;
}

static void lcd_restore_touch_button(void) {
    if (display_page == DISPLAY_PAGE_TOUCH)
        lcd_draw_button(92, 358, 136, 44, "CLEAR", 0);
    else if (display_page == DISPLAY_PAGE_BLUETOOTH)
        lcd_draw_button(72, 364, 176, 32, "SEND TEST", 0);
    else if (display_page == DISPLAY_PAGE_WIFI)
        lcd_draw_button(72, 364, 176, 32, "SCAN AP", 0);
}

static void lcd_init_ili9481(void) {
    lcd_write_cmd(0xFF);
    lcd_write_cmd(0xFF);
    delay_ms(5);
    lcd_write_cmd(0xFF);
    lcd_write_cmd(0xFF);
    lcd_write_cmd(0xFF);
    lcd_write_cmd(0xFF);
    delay_ms(10);

    lcd_write_cmd(0xB0); lcd_write_data(0x00);
    lcd_write_cmd(0xB3);
    lcd_write_data(0x02); lcd_write_data(0x00);
    lcd_write_data(0x00); lcd_write_data(0x00);
    lcd_write_cmd(0xC0);
    lcd_write_data(0x13); lcd_write_data(0x3B);
    lcd_write_data(0x00); lcd_write_data(0x00);
    lcd_write_data(0x00); lcd_write_data(0x01);
    lcd_write_data(0x00); lcd_write_data(0x43);
    lcd_write_cmd(0xC1);
    lcd_write_data(0x08); lcd_write_data(0x1B);
    lcd_write_data(0x08); lcd_write_data(0x08);
    lcd_write_cmd(0xC4);
    lcd_write_data(0x11); lcd_write_data(0x01);
    lcd_write_data(0x73); lcd_write_data(0x01);
    lcd_write_cmd(0xC6); lcd_write_data(0x00);

    static const uint8_t gamma[] = {
        0x0F, 0x05, 0x14, 0x5C, 0x03, 0x07, 0x07, 0x10, 0x00, 0x23,
        0x10, 0x07, 0x07, 0x53, 0x0C, 0x14, 0x05, 0x0F, 0x23, 0x00,
    };
    lcd_write_cmd(0xC8);
    for (unsigned i = 0; i < sizeof(gamma); i++)
        lcd_write_data(gamma[i]);

    lcd_write_cmd(0x35); lcd_write_data(0x00);
    lcd_write_cmd(0x44); lcd_write_data(0x00); lcd_write_data(0x01);
    lcd_write_cmd(0xD0);
    lcd_write_data(0x07); lcd_write_data(0x07);
    lcd_write_data(0x1D); lcd_write_data(0x03);
    lcd_write_cmd(0xD1);
    lcd_write_data(0x03); lcd_write_data(0x5B); lcd_write_data(0x10);
    lcd_write_cmd(0xD2);
    lcd_write_data(0x03); lcd_write_data(0x24); lcd_write_data(0x04);
    lcd_write_cmd(0x36); lcd_write_data(0x00);
    lcd_write_cmd(0x3A); lcd_write_data(0x55);
    lcd_write_cmd(0x11);
    delay_ms(150);
    lcd_write_cmd(0x29);
    delay_ms(30);
}

static void lcd_init_hx8357d(void) {
    static const uint8_t gamma[] = {
        0x0B, 0x11, 0x1E, 0x30, 0x3A, 0x43, 0x4E, 0x56,
        0x45, 0x3F, 0x39, 0x32, 0x2F, 0x2A, 0x29, 0x21,
        0x0B, 0x11, 0x1E, 0x30, 0x3A, 0x43, 0x4E, 0x56,
        0x45, 0x3F, 0x39, 0x32, 0x2F, 0x2A, 0x29, 0x21,
        0x00, 0x01,
    };

    lcd_write_cmd(0x11);
    delay_ms(150);

    lcd_write_cmd(0xB9);
    lcd_write_data(0xFF);
    lcd_write_data(0x83);
    lcd_write_data(0x57);
    delay_ms(5);

    lcd_write_cmd(0xB1);
    lcd_write_data(0x00);
    lcd_write_data(0x14);
    lcd_write_data(0x1C);
    lcd_write_data(0x1C);
    lcd_write_data(0xC3);
    lcd_write_data(0x44);
    lcd_write_data(0x70);

    lcd_write_cmd(0xB4);
    lcd_write_data(0x22);
    lcd_write_data(0x40);
    lcd_write_data(0x00);
    lcd_write_data(0x2A);
    lcd_write_data(0x2A);
    lcd_write_data(0x20);
    lcd_write_data(0x91);

    lcd_write_cmd(0x36);
    lcd_write_data(0x4C);

    lcd_write_cmd(0xC0);
    lcd_write_data(0x50);
    lcd_write_data(0x50);
    lcd_write_data(0x01);
    lcd_write_data(0x3C);
    lcd_write_data(0xC8);
    lcd_write_data(0x08);
    lcd_write_data(0x00);
    lcd_write_data(0x08);
    lcd_write_data(0x04);

    lcd_write_cmd(0xE0);
    for (unsigned i = 0; i < sizeof(gamma); i++)
        lcd_write_data(gamma[i]);

    lcd_write_cmd(0x3A);
    lcd_write_data(0x05);
    lcd_write_cmd(0x29);
    delay_ms(30);
}

static void lcd_init_ili9341(void) {
    lcd_write_cmd(0x01);
    delay_ms(20);
    lcd_write_cmd(0x28);
    lcd_write_cmd(0xCF);
    lcd_write_data(0x00); lcd_write_data(0xC1); lcd_write_data(0x30);
    lcd_write_cmd(0xED);
    lcd_write_data(0x64); lcd_write_data(0x03);
    lcd_write_data(0x12); lcd_write_data(0x81);
    lcd_write_cmd(0xE8);
    lcd_write_data(0x85); lcd_write_data(0x00); lcd_write_data(0x78);
    lcd_write_cmd(0xCB);
    lcd_write_data(0x39); lcd_write_data(0x2C); lcd_write_data(0x00);
    lcd_write_data(0x34); lcd_write_data(0x02);
    lcd_write_cmd(0xF7); lcd_write_data(0x20);
    lcd_write_cmd(0xEA); lcd_write_data(0x00); lcd_write_data(0x00);
    lcd_write_cmd(0xC0); lcd_write_data(0x23);
    lcd_write_cmd(0xC1); lcd_write_data(0x10);
    lcd_write_cmd(0xC5); lcd_write_data(0x3E); lcd_write_data(0x28);
    lcd_write_cmd(0xC7); lcd_write_data(0x86);
    lcd_write_cmd(0x36); lcd_write_data(0x48);
    lcd_write_cmd(0x3A); lcd_write_data(0x55);
    lcd_write_cmd(0xB1); lcd_write_data(0x00); lcd_write_data(0x18);
    lcd_write_cmd(0xB6); lcd_write_data(0x08); lcd_write_data(0x82);
    lcd_write_data(0x27);
    lcd_write_cmd(0xF2); lcd_write_data(0x00);
    lcd_write_cmd(0x26); lcd_write_data(0x01);
    lcd_write_cmd(0x11);
    delay_ms(120);
    lcd_write_cmd(0x29);
    delay_ms(20);
}

static void lcd_init_ili9325(void) {
    lcd_write_cmd(0xE5); lcd_write_data(0x78F0);
    lcd_write_cmd(0x01); lcd_write_data(0x0100);
    lcd_write_cmd(0x02); lcd_write_data(0x0700);
    lcd_write_cmd(0x03); lcd_write_data(0x1030);
    lcd_write_cmd(0x04); lcd_write_data(0x0000);
    lcd_write_cmd(0x08); lcd_write_data(0x0202);
    lcd_write_cmd(0x09); lcd_write_data(0x0000);
    lcd_write_cmd(0x0A); lcd_write_data(0x0000);
    lcd_write_cmd(0x10); lcd_write_data(0x0000);
    lcd_write_cmd(0x11); lcd_write_data(0x0007);
    lcd_write_cmd(0x12); lcd_write_data(0x0000);
    lcd_write_cmd(0x13); lcd_write_data(0x0000);
    delay_ms(50);
    lcd_write_cmd(0x10); lcd_write_data(0x1690);
    lcd_write_cmd(0x11); lcd_write_data(0x0227);
    delay_ms(50);
    lcd_write_cmd(0x12); lcd_write_data(0x001D);
    delay_ms(50);
    lcd_write_cmd(0x13); lcd_write_data(0x0800);
    lcd_write_cmd(0x29); lcd_write_data(0x0018);
    lcd_write_cmd(0x2B); lcd_write_data(0x000D);
    delay_ms(50);
    lcd_write_cmd(0x30); lcd_write_data(0x0000);
    lcd_write_cmd(0x31); lcd_write_data(0x0404);
    lcd_write_cmd(0x32); lcd_write_data(0x0003);
    lcd_write_cmd(0x35); lcd_write_data(0x0405);
    lcd_write_cmd(0x36); lcd_write_data(0x0808);
    lcd_write_cmd(0x37); lcd_write_data(0x0407);
    lcd_write_cmd(0x38); lcd_write_data(0x0303);
    lcd_write_cmd(0x39); lcd_write_data(0x0707);
    lcd_write_cmd(0x3C); lcd_write_data(0x0504);
    lcd_write_cmd(0x3D); lcd_write_data(0x0808);
    lcd_write_cmd(0x50); lcd_write_data(0x0000);
    lcd_write_cmd(0x51); lcd_write_data(0x00EF);
    lcd_write_cmd(0x52); lcd_write_data(0x0000);
    lcd_write_cmd(0x53); lcd_write_data(0x013F);
    lcd_write_cmd(0x60); lcd_write_data(0xA700);
    lcd_write_cmd(0x61); lcd_write_data(0x0001);
    lcd_write_cmd(0x6A); lcd_write_data(0x0000);
    lcd_write_cmd(0x80); lcd_write_data(0x0000);
    lcd_write_cmd(0x81); lcd_write_data(0x0000);
    lcd_write_cmd(0x82); lcd_write_data(0x0000);
    lcd_write_cmd(0x83); lcd_write_data(0x0000);
    lcd_write_cmd(0x84); lcd_write_data(0x0000);
    lcd_write_cmd(0x85); lcd_write_data(0x0000);
    lcd_write_cmd(0x90); lcd_write_data(0x0010);
    lcd_write_cmd(0x92); lcd_write_data(0x0000);
    lcd_write_cmd(0x93); lcd_write_data(0x0003);
    lcd_write_cmd(0x95); lcd_write_data(0x0110);
    lcd_write_cmd(0x97); lcd_write_data(0x0000);
    lcd_write_cmd(0x98); lcd_write_data(0x0000);
    lcd_write_cmd(0x07); lcd_write_data(0x0133);
}

static void lcd_gpio_fsmc_init(void) {
    RCC_APB2ENR |= (1U << 3) | (1U << 5) | (1U << 6) | (1U << 8);
    RCC_AHBENR |= 1U << 8;

    const unsigned pd[] = {0, 1, 4, 5, 8, 9, 10, 14, 15};
    const unsigned pe[] = {7, 8, 9, 10, 11, 12, 13, 14, 15};
    for (unsigned i = 0; i < sizeof(pd) / sizeof(pd[0]); i++)
        gpio_config_pin(&GPIOD_CRL, &GPIOD_CRH, pd[i], 0xBU);
    for (unsigned i = 0; i < sizeof(pe) / sizeof(pe[0]); i++)
        gpio_config_pin(&GPIOE_CRL, &GPIOE_CRH, pe[i], 0xBU);
    gpio_config_pin(&GPIOG_CRL, &GPIOG_CRH, 0, 0xBU);
    gpio_config_pin(&GPIOG_CRL, &GPIOG_CRH, 12, 0xBU);
    gpio_config_pin(&GPIOB_CRL, &GPIOB_CRH, LCD_BACKLIGHT_PIN, 0x3U);
    GPIOB_BSRR = 1U << LCD_BACKLIGHT_PIN;

    /* Bank1 NOR/SRAM4, 16-bit SRAM, write enabled, extended write timing. */
    FSMC_BCR4 = (1U << 14) | (1U << 12) | (1U << 4) | (1U << 0);
    FSMC_BTR4 = 1U | (15U << 8);
    FSMC_BWTR4 = 21U | (21U << 4) | (5U << 8);
}

int stm32_display_init(void) {
#ifndef CONFIG_STM32_XUANWU
    return -1;
#else
    lcd_gpio_fsmc_init();
    /*
     * The Xuanwu kit's supplied 3.5-inch panel is configured as HX8357DN in
     * the vendor project. Its ID read is unreliable on this 16-bit FSMC
     * adapter, so selecting a DCS fallback corrupts the two-byte pixel stream.
     */
    (void)lcd_read_id_hx8357d();
    display_controller_id = LCD_ID_HX8357D;
    lcd_init_hx8357d();
    (void)stm32_backlight_init();

    display_ready = 1;
    return display_controller_id;
#endif
}

int stm32_display_ready(void) {
    return display_ready;
}

void stm32_display_show_boot(void) {
    if (!display_ready)
        return;

    lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, COLOR_RED);
    delay_ms(180);
    lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, COLOR_GREEN);
    delay_ms(180);
    lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, COLOR_BLUE);
    delay_ms(180);

    lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, COLOR_DARK);
#ifdef CONFIG_STM32_LEGACY_DASHBOARD
    display_page = DISPLAY_PAGE_STATUS;
    display_last_second = (uint64_t)-1;
    lcd_draw_header();
    lcd_render_page();
#endif
}

void stm32_display_update_ticks(uint64_t ticks) {
    if (!display_ready)
        return;
    uint64_t seconds = ticks / 1000U;
    if (seconds == display_last_second)
        return;
    display_last_second = seconds;
    lcd_fill_rect(220, 44, 84, 24, COLOR_NAVY);
    lcd_draw_decimal(220, 48, seconds, COLOR_WHITE, COLOR_NAVY, 2);
    lcd_draw_text(292, 48, "S", COLOR_MUTED, COLOR_NAVY, 2);
}

void stm32_display_set_peripherals(int sram_ready, size_t sram_bytes,
                                   int sd_ready, uint64_t sd_sectors,
                                   int sd_fat32, int sd_bus_width,
                                   const char *sd_volume_label,
                                   int touch_ready, int keys_ready) {
    (void)sram_ready;
    (void)sram_bytes;
    display_sd_ready = sd_ready;
    display_sd_sectors = sd_sectors;
    display_sd_fat32 = sd_fat32;
    display_sd_bus_width = sd_bus_width;
    lcd_copy_label(sd_volume_label);
    display_touch_ready = touch_ready;
    display_keys_ready = keys_ready;
    lcd_redraw_peripheral_status();
}

void stm32_display_set_bluetooth(int ready, int detected, int at_responsive,
                                 int connected, int waiting, int configured,
                                 int slave_mode,
                                 int uuid_supported, int uuid_configured,
                                 const char *device_name,
                                 const char *pin, uint16_t service_uuid,
                                 uint32_t baud_rate, uint32_t received_bytes,
                                 uint32_t transmitted_bytes,
                                 uint32_t dropped_bytes) {
    int changed = display_bluetooth_ready != ready ||
                  display_bluetooth_detected != detected ||
                  display_bluetooth_at_responsive != at_responsive ||
                  display_bluetooth_connected != connected ||
                  display_bluetooth_waiting != waiting ||
                  display_bluetooth_configured != configured ||
                  display_bluetooth_slave != slave_mode ||
                  display_bluetooth_uuid_supported != uuid_supported ||
                  display_bluetooth_uuid_configured != uuid_configured ||
                  display_bluetooth_uuid != service_uuid ||
                  display_bluetooth_baud != baud_rate ||
                  display_bluetooth_rx_bytes != received_bytes ||
                  display_bluetooth_tx_bytes != transmitted_bytes ||
                  display_bluetooth_dropped != dropped_bytes;
    char previous_name[sizeof(display_bluetooth_name)];
    char previous_pin[sizeof(display_bluetooth_pin)];
    for (unsigned i = 0; i < sizeof(previous_name); i++)
        previous_name[i] = display_bluetooth_name[i];
    for (unsigned i = 0; i < sizeof(previous_pin); i++)
        previous_pin[i] = display_bluetooth_pin[i];

    display_bluetooth_ready = ready;
    display_bluetooth_detected = detected;
    display_bluetooth_at_responsive = at_responsive;
    display_bluetooth_connected = connected;
    display_bluetooth_waiting = waiting;
    display_bluetooth_configured = configured;
    display_bluetooth_slave = slave_mode;
    display_bluetooth_uuid_supported = uuid_supported;
    display_bluetooth_uuid_configured = uuid_configured;
    display_bluetooth_uuid = service_uuid;
    display_bluetooth_baud = baud_rate;
    display_bluetooth_rx_bytes = received_bytes;
    display_bluetooth_tx_bytes = transmitted_bytes;
    display_bluetooth_dropped = dropped_bytes;
    lcd_copy_short_text(display_bluetooth_name,
                        sizeof(display_bluetooth_name), device_name);
    lcd_copy_short_text(display_bluetooth_pin,
                        sizeof(display_bluetooth_pin), pin);
    for (unsigned i = 0; i < sizeof(previous_name); i++)
        if (previous_name[i] != display_bluetooth_name[i])
            changed = 1;
    for (unsigned i = 0; i < sizeof(previous_pin); i++)
        if (previous_pin[i] != display_bluetooth_pin[i])
            changed = 1;
    if (changed)
        lcd_redraw_peripheral_status();
}

void stm32_display_set_wifi(int active, int detected, int at_responsive,
                            int configured, int connecting, int joined,
                            int got_ip, int socket_connected,
                            const char *ssid, const char *ip_address,
                            const char *mac_address, const char *scan_ssid,
                            uint32_t access_points, uint32_t baud_rate,
                            uint32_t received_bytes,
                            uint32_t transmitted_bytes,
                            uint32_t dropped_bytes, const char *last_event) {
    int changed = display_wifi_active != active ||
                  display_wifi_detected != detected ||
                  display_wifi_at_responsive != at_responsive ||
                  display_wifi_configured != configured ||
                  display_wifi_connecting != connecting ||
                  display_wifi_joined != joined ||
                  display_wifi_got_ip != got_ip ||
                  display_wifi_socket_connected != socket_connected ||
                  display_wifi_access_points != access_points ||
                  display_wifi_baud != baud_rate ||
                  display_wifi_rx_bytes != received_bytes ||
                  display_wifi_tx_bytes != transmitted_bytes ||
                  display_wifi_dropped != dropped_bytes;
    char old_ssid[sizeof(display_wifi_ssid)];
    char old_ip[sizeof(display_wifi_ip)];
    char old_mac[sizeof(display_wifi_mac)];
    char old_scan[sizeof(display_wifi_scan_ssid)];
    char old_event[sizeof(display_wifi_event)];
    memcpy(old_ssid, display_wifi_ssid, sizeof(old_ssid));
    memcpy(old_ip, display_wifi_ip, sizeof(old_ip));
    memcpy(old_mac, display_wifi_mac, sizeof(old_mac));
    memcpy(old_scan, display_wifi_scan_ssid, sizeof(old_scan));
    memcpy(old_event, display_wifi_event, sizeof(old_event));

    display_wifi_active = active;
    display_wifi_detected = detected;
    display_wifi_at_responsive = at_responsive;
    display_wifi_configured = configured;
    display_wifi_connecting = connecting;
    display_wifi_joined = joined;
    display_wifi_got_ip = got_ip;
    display_wifi_socket_connected = socket_connected;
    display_wifi_access_points = access_points;
    display_wifi_baud = baud_rate;
    display_wifi_rx_bytes = received_bytes;
    display_wifi_tx_bytes = transmitted_bytes;
    display_wifi_dropped = dropped_bytes;
    lcd_copy_short_text(display_wifi_ssid, sizeof(display_wifi_ssid), ssid);
    lcd_copy_short_text(display_wifi_ip, sizeof(display_wifi_ip), ip_address);
    lcd_copy_short_text(display_wifi_mac, sizeof(display_wifi_mac), mac_address);
    lcd_copy_short_text(display_wifi_scan_ssid,
                        sizeof(display_wifi_scan_ssid), scan_ssid);
    lcd_copy_short_text(display_wifi_event,
                        sizeof(display_wifi_event), last_event);
    if (memcmp(old_ssid, display_wifi_ssid, sizeof(old_ssid)) != 0 ||
        memcmp(old_ip, display_wifi_ip, sizeof(old_ip)) != 0 ||
        memcmp(old_mac, display_wifi_mac, sizeof(old_mac)) != 0 ||
        memcmp(old_scan, display_wifi_scan_ssid, sizeof(old_scan)) != 0 ||
        memcmp(old_event, display_wifi_event, sizeof(old_event)) != 0)
        changed = 1;
    if (changed && display_ready &&
        (display_page == DISPLAY_PAGE_WIFI ||
         display_page == DISPLAY_PAGE_STATUS))
        lcd_render_page();
}

void stm32_display_set_memory(const stm32_memory_info_t *info) {
    if (!info)
        return;

    int changed =
        display_memory.internal_ram_total != info->internal_ram_total ||
        display_memory.internal_ram_used != info->internal_ram_used ||
        display_memory.internal_heap_total != info->internal_heap_total ||
        display_memory.internal_heap_used != info->internal_heap_used ||
        display_memory.stack_peak_bytes != info->stack_peak_bytes ||
        display_memory.external_ram_total != info->external_ram_total ||
        display_memory.external_ram_used != info->external_ram_used ||
        display_memory.flash_total != info->flash_total ||
        display_memory.flash_used != info->flash_used ||
        display_memory.flash_capacity_from_silicon !=
            info->flash_capacity_from_silicon ||
        display_memory.ram_capacity_from_silicon !=
            info->ram_capacity_from_silicon ||
        display_memory.silicon_capacity_valid !=
            info->silicon_capacity_valid;
    display_memory = *info;
    if (changed)
        lcd_redraw_peripheral_status();
}

void stm32_display_set_light(int ready, uint16_t raw_adc,
                             uint8_t intensity_percent,
                             uint8_t backlight_percent,
                             int auto_brightness) {
    int status_changed =
        display_light_ready != ready ||
        display_auto_brightness != auto_brightness;
    int changed =
        status_changed ||
        display_light_raw != raw_adc ||
        display_light_percent != intensity_percent ||
        display_backlight_percent != backlight_percent;

    display_light_ready = ready;
    display_light_raw = raw_adc;
    display_light_percent = intensity_percent;
    display_backlight_percent = backlight_percent;
    display_auto_brightness = auto_brightness;
    if (!changed || !display_ready || display_page != DISPLAY_PAGE_STATUS)
        return;
    if (status_changed)
        lcd_draw_status_page();
    else
        lcd_draw_light_values(1);
}

void stm32_display_show_bluetooth_line(const char *line) {
    unsigned i = 0;

    if (line) {
        while (i + 1U < sizeof(display_bluetooth_line) && line[i] &&
               line[i] != '\r' && line[i] != '\n') {
            char c = line[i];
            display_bluetooth_line[i] =
                c >= 32 && c <= 126 ? c : '.';
            i++;
        }
    }
    display_bluetooth_line[i] = '\0';
    while (++i < sizeof(display_bluetooth_line))
        display_bluetooth_line[i] = '\0';
    if (display_ready && display_page == DISPLAY_PAGE_BLUETOOTH)
        lcd_draw_bluetooth_page();
}

void stm32_display_show_touch(uint16_t x, uint16_t y, int pressed) {
    if (!display_ready)
        return;
    if (!pressed && !display_touch_pressed)
        return;

    int was_pressed = display_touch_pressed;
    display_touch_pressed = pressed;
    if (!pressed) {
        display_draw_valid = 0;
        lcd_redraw_touch_released();
        lcd_restore_touch_button();
        return;
    }

    display_touch_x = x;
    display_touch_y = y;
    lcd_redraw_touch_status();
    lcd_handle_touch_down(x, y, !was_pressed);
}

void stm32_display_handle_key(stm32_key_t key) {
    if (!display_ready)
        return;

    if (key == STM32_KEY_LEFT) {
        lcd_select_relative_page(-1);
        return;
    }
    if (key == STM32_KEY_RIGHT) {
        if (display_page == DISPLAY_PAGE_STATUS) {
            if (display_status_selection == 0U)
                lcd_select_page(DISPLAY_PAGE_MEMORY);
            else if (display_status_selection == 1U)
                lcd_select_page(DISPLAY_PAGE_STORAGE);
            else if (display_status_selection == 2U)
                lcd_select_page(DISPLAY_PAGE_BLUETOOTH);
            else if (display_status_selection == 3U)
                lcd_select_page(DISPLAY_PAGE_TOUCH);
            else if (display_status_selection == 5U)
                lcd_select_relative_page(1);
            return;
        }
        lcd_select_relative_page(1);
        return;
    }

    if (display_page == DISPLAY_PAGE_STATUS) {
        if (key == STM32_KEY_UP) {
            display_status_selection =
                display_status_selection == 0U ? 5U
                                               : display_status_selection - 1U;
        } else if (key == STM32_KEY_DOWN) {
            display_status_selection = (display_status_selection + 1U) % 6U;
        } else {
            return;
        }
        lcd_draw_status_page();
        return;
    }

    if (display_page == DISPLAY_PAGE_STORAGE) {
        if (key == STM32_KEY_UP)
            lcd_select_page(DISPLAY_PAGE_STATUS);
        else if (key == STM32_KEY_DOWN)
            lcd_render_page();
        return;
    }

    if (display_page == DISPLAY_PAGE_BLUETOOTH) {
        if (key == STM32_KEY_UP) {
            display_pending_action =
                STM32_DISPLAY_ACTION_BLUETOOTH_TEST;
            lcd_draw_button(72, 364, 176, 32, "SEND TEST", 1);
        } else if (key == STM32_KEY_DOWN) {
            stm32_display_show_bluetooth_line(NULL);
        }
        return;
    }

    if (display_page == DISPLAY_PAGE_WIFI) {
        if (key == STM32_KEY_UP) {
            display_pending_action = STM32_DISPLAY_ACTION_WIFI_SCAN;
            lcd_draw_button(72, 364, 176, 32, "SCAN AP", 1);
        } else if (key == STM32_KEY_DOWN) {
            lcd_render_page();
        }
        return;
    }

    if (display_page != DISPLAY_PAGE_TOUCH)
        return;
    if (key == STM32_KEY_DOWN) {
        lcd_clear_touch_canvas();
        lcd_draw_key_cursor();
        lcd_draw_button(92, 358, 136, 44, "CLEAR", 1);
        return;
    }
    if (key != STM32_KEY_UP)
        return;

    display_key_cursor_y -= UI_CURSOR_STEP;
    if (display_key_cursor_y <= UI_TOUCH_Y + 6U)
        display_key_cursor_y = UI_TOUCH_Y + UI_TOUCH_H - 7U;
    display_touch_x = display_key_cursor_x;
    display_touch_y = display_key_cursor_y;
    lcd_draw_touch_coordinates();
    lcd_clear_touch_canvas();
    lcd_draw_key_cursor();
}

stm32_display_action_t stm32_display_take_action(void) {
    stm32_display_action_t action = display_pending_action;
    display_pending_action = STM32_DISPLAY_ACTION_NONE;
    if (action == STM32_DISPLAY_ACTION_BLUETOOTH_TEST &&
        display_ready && display_page == DISPLAY_PAGE_BLUETOOTH)
        lcd_draw_button(72, 364, 176, 32, "SEND TEST", 0);
    else if (action == STM32_DISPLAY_ACTION_WIFI_SCAN &&
             display_ready && display_page == DISPLAY_PAGE_WIFI)
        lcd_draw_button(72, 364, 176, 32, "SCAN AP", 0);
    return action;
}

#endif
