#ifdef CONFIG_BOARD_STM32F103

#include "display.h"

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

static uint64_t display_last_second = (uint64_t)-1;
static uint16_t display_controller_id;
static int display_ready;

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
    lcd_gpio_fsmc_init();
    /*
     * The Xuanwu kit's supplied 3.5-inch panel is configured as HX8357DN in
     * the vendor project. Its ID read is unreliable on this 16-bit FSMC
     * adapter, so selecting a DCS fallback corrupts the two-byte pixel stream.
     */
    (void)lcd_read_id_hx8357d();
    display_controller_id = LCD_ID_HX8357D;
    lcd_init_hx8357d();

    display_ready = 1;
    return display_controller_id;
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
    lcd_fill_rect(0, 0, LCD_WIDTH, 86, COLOR_NAVY);
    lcd_fill_rect(0, 82, LCD_WIDTH, 4, COLOR_CYAN);
    lcd_draw_text(24, 18, "A20OS", COLOR_WHITE, COLOR_NAVY, 7);
    lcd_draw_text(26, 66, "ARMV7-M / STM32F103", COLOR_MUTED,
                  COLOR_NAVY, 2);

    lcd_draw_text(24, 122, "KERNEL BRINGUP", COLOR_CYAN, COLOR_DARK, 3);
    lcd_draw_text(24, 174, "CPU", COLOR_MUTED, COLOR_DARK, 2);
    lcd_draw_text(132, 174, "CORTEX-M3", COLOR_WHITE, COLOR_DARK, 2);
    lcd_draw_text(24, 208, "MEMORY", COLOR_MUTED, COLOR_DARK, 2);
    lcd_draw_text(132, 208, "64K SRAM", COLOR_WHITE, COLOR_DARK, 2);
    lcd_draw_text(24, 242, "TIMER", COLOR_MUTED, COLOR_DARK, 2);
    lcd_draw_text(132, 242, "SYSTICK 1KHZ", COLOR_WHITE, COLOR_DARK, 2);
    lcd_draw_text(24, 276, "LCD ID", COLOR_MUTED, COLOR_DARK, 2);
    lcd_draw_decimal(132, 276, display_controller_id,
                     COLOR_YELLOW, COLOR_DARK, 2);

    lcd_fill_rect(24, 326, 272, 64, COLOR_NAVY);
    lcd_fill_rect(24, 326, 8, 64, COLOR_GREEN);
    lcd_draw_text(48, 340, "SYSTEM ONLINE", COLOR_GREEN, COLOR_NAVY, 3);

    lcd_draw_text(24, 416, "UPTIME", COLOR_MUTED, COLOR_DARK, 2);
    lcd_draw_text(24, 452, "A20OS MCU PORT", COLOR_MUTED, COLOR_DARK, 2);
}

void stm32_display_update_ticks(uint64_t ticks) {
    if (!display_ready)
        return;
    uint64_t seconds = ticks / 1000U;
    if (seconds == display_last_second)
        return;
    display_last_second = seconds;
    lcd_fill_rect(126, 406, 170, 32, COLOR_DARK);
    lcd_draw_decimal(126, 416, seconds, COLOR_WHITE, COLOR_DARK, 2);
    lcd_draw_text(260, 416, "S", COLOR_WHITE, COLOR_DARK, 2);
}

#endif
