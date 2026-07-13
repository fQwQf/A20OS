#if 1

#include "lv_port_indev.h"
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include "lvgl.h"

struct input_event {
    uint32_t time_sec;
    uint32_t time_usec;
    uint16_t type;
    uint16_t code;
    int32_t value;
};

#define EV_KEY 0x01
#define EV_REL 0x02

#define REL_X 0x00
#define REL_Y 0x01

#define BTN_LEFT     0x110

static int ev_fd = -1;
static int32_t mouse_x = 0;
static int32_t mouse_y = 0;
static bool mouse_pressed = false;
static bool left_shift = false;
static bool right_shift = false;
static bool left_ctrl = false;
static bool right_ctrl = false;
static bool caps_lock = false;
static lv_port_key_handler_t key_handler;
static void * key_handler_data;

enum {
    KEY_ESC = 1,
    KEY_1 = 2,
    KEY_2,
    KEY_3,
    KEY_4,
    KEY_5,
    KEY_6,
    KEY_7,
    KEY_8,
    KEY_9,
    KEY_0,
    KEY_MINUS,
    KEY_EQUAL,
    KEY_BACKSPACE,
    KEY_TAB,
    KEY_Q,
    KEY_W,
    KEY_E,
    KEY_R,
    KEY_T,
    KEY_Y,
    KEY_U,
    KEY_I,
    KEY_O,
    KEY_P,
    KEY_LEFTBRACE,
    KEY_RIGHTBRACE,
    KEY_ENTER,
    KEY_LEFTCTRL,
    KEY_A,
    KEY_S,
    KEY_D,
    KEY_F,
    KEY_G,
    KEY_H,
    KEY_J,
    KEY_K,
    KEY_L,
    KEY_SEMICOLON,
    KEY_APOSTROPHE,
    KEY_GRAVE,
    KEY_LEFTSHIFT,
    KEY_BACKSLASH,
    KEY_Z,
    KEY_X,
    KEY_C,
    KEY_V,
    KEY_B,
    KEY_N,
    KEY_M,
    KEY_COMMA,
    KEY_DOT,
    KEY_SLASH,
    KEY_RIGHTSHIFT,
    KEY_SPACE = 57,
    KEY_CAPSLOCK,
    KEY_HOME = 102,
    KEY_UP,
    KEY_PAGEUP,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_END,
    KEY_DOWN,
    KEY_PAGEDOWN,
    KEY_INSERT,
    KEY_DELETE,
    KEY_RIGHTCTRL = 97,
};

typedef struct {
    uint32_t key;
    lv_indev_state_t state;
} queued_key_t;

#define KEY_QUEUE_SIZE 128
static queued_key_t key_queue[KEY_QUEUE_SIZE];
static uint32_t key_queue_head;
static uint32_t key_queue_tail;
static uint32_t pressed_keys[256];

static bool key_queue_push(uint32_t key, lv_indev_state_t state)
{
    uint32_t next = (key_queue_head + 1) % KEY_QUEUE_SIZE;
    if (next == key_queue_tail)
        return false;

    key_queue[key_queue_head].key = key;
    key_queue[key_queue_head].state = state;
    key_queue_head = next;
    return true;
}

static bool key_queue_pop(queued_key_t * key)
{
    if (key_queue_head == key_queue_tail)
        return false;

    *key = key_queue[key_queue_tail];
    key_queue_tail = (key_queue_tail + 1) % KEY_QUEUE_SIZE;
    return true;
}

static uint32_t map_key_code(uint16_t code)
{
    static const char letters[] = {
        [KEY_Q] = 'q', [KEY_W] = 'w', [KEY_E] = 'e', [KEY_R] = 'r',
        [KEY_T] = 't', [KEY_Y] = 'y', [KEY_U] = 'u', [KEY_I] = 'i',
        [KEY_O] = 'o', [KEY_P] = 'p', [KEY_A] = 'a', [KEY_S] = 's',
        [KEY_D] = 'd', [KEY_F] = 'f', [KEY_G] = 'g', [KEY_H] = 'h',
        [KEY_J] = 'j', [KEY_K] = 'k', [KEY_L] = 'l', [KEY_Z] = 'z',
        [KEY_X] = 'x', [KEY_C] = 'c', [KEY_V] = 'v', [KEY_B] = 'b',
        [KEY_N] = 'n', [KEY_M] = 'm',
    };
    static const char normal[] = {
        [KEY_1] = '1', [KEY_2] = '2', [KEY_3] = '3', [KEY_4] = '4',
        [KEY_5] = '5', [KEY_6] = '6', [KEY_7] = '7', [KEY_8] = '8',
        [KEY_9] = '9', [KEY_0] = '0', [KEY_MINUS] = '-',
        [KEY_EQUAL] = '=', [KEY_LEFTBRACE] = '[', [KEY_RIGHTBRACE] = ']',
        [KEY_SEMICOLON] = ';', [KEY_APOSTROPHE] = '\'', [KEY_GRAVE] = '`',
        [KEY_BACKSLASH] = '\\', [KEY_COMMA] = ',', [KEY_DOT] = '.',
        [KEY_SLASH] = '/',
    };
    static const char shifted[] = {
        [KEY_1] = '!', [KEY_2] = '@', [KEY_3] = '#', [KEY_4] = '$',
        [KEY_5] = '%', [KEY_6] = '^', [KEY_7] = '&', [KEY_8] = '*',
        [KEY_9] = '(', [KEY_0] = ')', [KEY_MINUS] = '_',
        [KEY_EQUAL] = '+', [KEY_LEFTBRACE] = '{', [KEY_RIGHTBRACE] = '}',
        [KEY_SEMICOLON] = ':', [KEY_APOSTROPHE] = '"', [KEY_GRAVE] = '~',
        [KEY_BACKSLASH] = '|', [KEY_COMMA] = '<', [KEY_DOT] = '>',
        [KEY_SLASH] = '?',
    };
    bool shift = left_shift || right_shift;
    bool ctrl = left_ctrl || right_ctrl;

    if (code < sizeof(letters) && letters[code]) {
        uint32_t key = (uint32_t)letters[code];
        if (ctrl)
            return key - 'a' + 1;
        if (shift != caps_lock)
            key -= 'a' - 'A';
        return key;
    }

    if (code < sizeof(normal) && normal[code])
        return (uint32_t)(shift ? shifted[code] : normal[code]);

    switch (code) {
    case KEY_ESC: return 27;
    case KEY_BACKSPACE: return 127;
    case KEY_TAB: return 9;
    case KEY_ENTER: return 10;
    case KEY_SPACE: return ' ';
    case KEY_UP: return LV_PORT_KEY_UP;
    case KEY_DOWN: return LV_PORT_KEY_DOWN;
    case KEY_RIGHT: return LV_PORT_KEY_RIGHT;
    case KEY_LEFT: return LV_PORT_KEY_LEFT;
    case KEY_DELETE: return LV_PORT_KEY_DELETE;
    case KEY_HOME: return LV_PORT_KEY_HOME;
    case KEY_END: return LV_PORT_KEY_END;
    case KEY_PAGEUP: return LV_PORT_KEY_PAGE_UP;
    case KEY_PAGEDOWN: return LV_PORT_KEY_PAGE_DOWN;
    default: return 0;
    }
}

static uint32_t to_lv_key(uint32_t key)
{
    switch (key) {
    case LV_PORT_KEY_UP: return LV_KEY_UP;
    case LV_PORT_KEY_DOWN: return LV_KEY_DOWN;
    case LV_PORT_KEY_RIGHT: return LV_KEY_RIGHT;
    case LV_PORT_KEY_LEFT: return LV_KEY_LEFT;
    case LV_PORT_KEY_DELETE: return LV_KEY_DEL;
    case LV_PORT_KEY_HOME: return LV_KEY_HOME;
    case LV_PORT_KEY_END: return LV_KEY_END;
    case LV_PORT_KEY_PAGE_UP: return LV_KEY_UP;
    case LV_PORT_KEY_PAGE_DOWN: return LV_KEY_DOWN;
    default: return key;
    }
}

static void process_key_event(const struct input_event * ev)
{
    bool pressed = ev->value != 0;
    bool repeated = ev->value == 2;
    uint16_t code = ev->code;

    switch (code) {
    case KEY_LEFTSHIFT: left_shift = pressed; return;
    case KEY_RIGHTSHIFT: right_shift = pressed; return;
    case KEY_LEFTCTRL: left_ctrl = pressed; return;
    case KEY_RIGHTCTRL: right_ctrl = pressed; return;
    case KEY_CAPSLOCK:
        if (ev->value == 1)
            caps_lock = !caps_lock;
        return;
    default:
        break;
    }

    uint32_t key = 0;
    if (pressed) {
        key = map_key_code(code);
        if (code < sizeof(pressed_keys) / sizeof(pressed_keys[0]))
            pressed_keys[code] = key;
    } else if (code < sizeof(pressed_keys) / sizeof(pressed_keys[0])) {
        key = pressed_keys[code];
        pressed_keys[code] = 0;
    }

    if (!key)
        return;

    if (pressed && key_handler)
        key_handler(key, key_handler_data);

    if (repeated) {
        key_queue_push(to_lv_key(key), LV_INDEV_STATE_RELEASED);
        key_queue_push(to_lv_key(key), LV_INDEV_STATE_PRESSED);
    } else {
        key_queue_push(to_lv_key(key),
                       pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED);
    }
}

static void mouse_init(void)
{
    lv_display_t * display = lv_display_get_default();
    if (display != NULL) {
        mouse_x = lv_display_get_horizontal_resolution(display) / 2;
        mouse_y = lv_display_get_vertical_resolution(display) / 2;
    }

    ev_fd = open("/dev/event0", O_RDONLY | O_NONBLOCK);
    if (ev_fd < 0) {
        printf("Failed to open /dev/event0\n");
    } else {
        printf("Opened /dev/event0 for input\n");
    }
}

static void poll_events(void)
{
    if (ev_fd < 0)
        return;

    struct input_event ev;
    while (read(ev_fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (ev.type == EV_REL) {
            if (ev.code == REL_X) {
                mouse_x += (int32_t)ev.value;
            } else if (ev.code == REL_Y) {
                mouse_y += (int32_t)ev.value;
            }
        } else if (ev.type == EV_KEY) {
            if (ev.code == BTN_LEFT) {
                mouse_pressed = (ev.value != 0);
            } else {
                process_key_event(&ev);
            }
        }
    }
}

static void mouse_read(lv_indev_t * indev_drv, lv_indev_data_t * data)
{
    (void)indev_drv;
    poll_events();

    lv_display_t * display = lv_display_get_default();
    int32_t max_x = display != NULL ? lv_display_get_horizontal_resolution(display) : 1024;
    int32_t max_y = display != NULL ? lv_display_get_vertical_resolution(display) : 768;

    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x >= max_x) mouse_x = max_x - 1;
    if (mouse_y >= max_y) mouse_y = max_y - 1;

    data->point.x = mouse_x;
    data->point.y = mouse_y;
    data->state = mouse_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

lv_indev_t * lv_port_indev_init(void)
{
    mouse_init();

    lv_indev_t * indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, mouse_read);

    return indev;
}

static void keyboard_read(lv_indev_t * indev_drv, lv_indev_data_t * data)
{
    (void)indev_drv;
    queued_key_t key;

    poll_events();
    if (key_queue_pop(&key)) {
        data->key = key.key;
        data->state = key.state;
        data->continue_reading = key_queue_head != key_queue_tail;
    } else {
        data->key = 0;
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

lv_indev_t * lv_port_keyboard_init(void)
{
    lv_indev_t * indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(indev, keyboard_read);
    return indev;
}

void lv_port_indev_set_key_handler(lv_port_key_handler_t handler, void * user_data)
{
    key_handler = handler;
    key_handler_data = user_data;
}

#endif
