#ifdef CONFIG_BOARD_STM32F103

#include "device_model.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_register.h"
#include "drivers/stm32f1/bluetooth.h"
#include "drivers/stm32f1/wifi.h"
#include "drivers/stm32f1/display.h"
#include "drivers/stm32f1/touch.h"
#include "drivers/stm32f1/dht11.h"
#include "core/string.h"

extern int bus_register(bus_type_t *) __attribute__((weak));
extern int device_register(device_t *) __attribute__((weak));

static bus_type_t stm32_bus = { .name = "stm32-platform" };
static int published;

static int model_match(device_t *dev, const driver_t *drv) {
    return dev && drv && dev->bus == drv->bus &&
           dev->name && drv->name && strcmp(dev->name, drv->name) == 0;
}

static int probe_bt(device_t *d) {
    if (!stm32_bluetooth_info()->ready) return -1;
    d->drv_priv = (void *)stm32_bluetooth_info(); return 0;
}
static int remove_bt(device_t *d) { (void)d; return 0; }
static int probe_wifi(device_t *d) {
    if (!stm32_wifi_info()->active) return -1;
    d->drv_priv = (void *)stm32_wifi_info(); return 0;
}
static int remove_wifi(device_t *d) { stm32_wifi_shutdown(); (void)d; return 0; }
static int bt_read(struct device *d, void *buf, size_t n) {
    (void)d; return stm32_bluetooth_read_line((char *)buf, n);
}
static int bt_write(struct device *d, const void *buf, size_t n) {
    (void)d; return stm32_bluetooth_send(buf, n);
}
static int bt_ioctl(struct device *d, unsigned long req, void *arg) {
    (void)d; (void)req; (void)arg; return -1;
}
static int bt_poll(struct device *d, short events) {
    (void)d; (void)events; return stm32_bluetooth_info()->ready;
}
static char_dev_ops_t bt_ops = {
    .read = bt_read, .write = bt_write, .ioctl = bt_ioctl, .poll = bt_poll,
};
static int wifi_open(struct device *d) { (void)d; return 0; }
static int wifi_stop(struct device *d) { (void)d; stm32_wifi_shutdown(); return 0; }
static int wifi_send(struct device *d, const void *p, size_t n) {
    (void)d; return stm32_wifi_send(p, n);
}
static int wifi_recv(struct device *d, void *p, size_t n) {
    (void)d; return stm32_wifi_read(p, n);
}
static void wifi_poll(struct device *d) { (void)d; }
static net_dev_ops_t wifi_ops = {
    .open = wifi_open, .stop = wifi_stop, .send = wifi_send,
    .recv = wifi_recv, .poll = wifi_poll,
};
static int probe_display(device_t *d) {
    if (!stm32_display_ready()) return -1;
    d->drv_priv = (void *)1; return 0;
}
static int probe_touch(device_t *d) {
    if (!stm32_touch_ready()) return -1;
    d->drv_priv = (void *)1; return 0;
}
static int probe_dht(device_t *d) {
    d->drv_priv = (void *)1; return 0;
}

#define MODEL_DRIVER(n, p, r, c) \
    static driver_t n = { .name = #n, .bus = &stm32_bus, .probe = p, \
                          .remove = r, .class_type = c }

MODEL_DRIVER(stm32_bluetooth, probe_bt, remove_bt, DEV_CLASS_CHAR);
MODEL_DRIVER(stm32_wifi, probe_wifi, remove_wifi, DEV_CLASS_NET);
MODEL_DRIVER(stm32_display, probe_display, remove_bt, DEV_CLASS_DISPLAY);
MODEL_DRIVER(stm32_touch, probe_touch, remove_bt, DEV_CLASS_INPUT);
MODEL_DRIVER(stm32_dht11, probe_dht, remove_bt, DEV_CLASS_INPUT);

DRIVER_REGISTER(stm32_bluetooth);
DRIVER_REGISTER(stm32_wifi);
DRIVER_REGISTER(stm32_display);
DRIVER_REGISTER(stm32_touch);
DRIVER_REGISTER(stm32_dht11);

static void model_bind_class_ops(void) {
    stm32_bluetooth.class_ops = &bt_ops;
    stm32_wifi.class_ops = &wifi_ops;
}

static device_t devices[] = {
    { .name = "stm32_bluetooth", .bus = &stm32_bus },
    { .name = "stm32_wifi",      .bus = &stm32_bus },
    { .name = "stm32_display",   .bus = &stm32_bus },
    { .name = "stm32_touch",     .bus = &stm32_bus },
    { .name = "stm32_dht11",     .bus = &stm32_bus },
};

void stm32_device_model_publish(void) {
    if (published || !bus_register || !device_register) return;
    model_bind_class_ops();
    stm32_bus.match = model_match;
    bus_register(&stm32_bus);
    for (unsigned i = 0; i < sizeof(devices) / sizeof(devices[0]); i++)
        (void)device_register(&devices[i]);
    published = 1;
}

#endif
