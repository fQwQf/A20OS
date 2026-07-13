#include "desktop_terminal.h"
#include "lv_port_indev.h"

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

enum {
    TERM_COLS = 76,
    TERM_ROWS = 34,
    TERM_CELL_WIDTH = 8,
    TERM_CELL_HEIGHT = 16,
};

typedef struct {
    int master_fd;
    pid_t shell_pid;
    char pty_name[32];
    char cells[TERM_ROWS][TERM_COLS];
    char rendered[(TERM_COLS + 1) * TERM_ROWS + 1];
    int row;
    int col;
    int saved_row;
    int saved_col;
    int escape_state;
    int csi_args[8];
    int csi_argc;
    bool dirty;
    lv_obj_t * output;
    lv_obj_t * cursor;
    lv_obj_t * status;
    lv_obj_t * live;
    lv_obj_t * panel;
} terminal_state_t;

static terminal_state_t terminal = {
    .master_fd = -1,
    .shell_pid = -1,
};

static void terminal_reset_cells(void)
{
    memset(terminal.cells, ' ', sizeof(terminal.cells));
    terminal.row = 0;
    terminal.col = 0;
    terminal.saved_row = 0;
    terminal.saved_col = 0;
    terminal.escape_state = 0;
    terminal.csi_argc = 0;
    terminal.dirty = true;
}

static void terminal_scroll(void)
{
    memmove(terminal.cells[0], terminal.cells[1],
            (TERM_ROWS - 1) * TERM_COLS);
    memset(terminal.cells[TERM_ROWS - 1], ' ', TERM_COLS);
    terminal.row = TERM_ROWS - 1;
    terminal.dirty = true;
}

static void terminal_newline(void)
{
    terminal.row++;
    if (terminal.row >= TERM_ROWS)
        terminal_scroll();
}

static void terminal_clear_line(int mode)
{
    if (mode == 1) {
        memset(terminal.cells[terminal.row], ' ', terminal.col + 1);
    } else if (mode == 2) {
        memset(terminal.cells[terminal.row], ' ', TERM_COLS);
    } else {
        memset(&terminal.cells[terminal.row][terminal.col], ' ',
               TERM_COLS - terminal.col);
    }
    terminal.dirty = true;
}

static void terminal_clear_screen(int mode)
{
    if (mode == 2 || mode == 3) {
        memset(terminal.cells, ' ', sizeof(terminal.cells));
        terminal.row = 0;
        terminal.col = 0;
    } else if (mode == 1) {
        for (int row = 0; row < terminal.row; row++)
            memset(terminal.cells[row], ' ', TERM_COLS);
        memset(terminal.cells[terminal.row], ' ', terminal.col + 1);
    } else {
        memset(&terminal.cells[terminal.row][terminal.col], ' ',
               TERM_COLS - terminal.col);
        for (int row = terminal.row + 1; row < TERM_ROWS; row++)
            memset(terminal.cells[row], ' ', TERM_COLS);
    }
    terminal.dirty = true;
}

static int csi_arg(int index, int fallback)
{
    if (index >= terminal.csi_argc || terminal.csi_args[index] == 0)
        return fallback;
    return terminal.csi_args[index];
}

static void terminal_handle_csi(char command)
{
    int amount = csi_arg(0, 1);

    switch (command) {
    case 'A':
        terminal.row -= amount;
        if (terminal.row < 0) terminal.row = 0;
        break;
    case 'B':
        terminal.row += amount;
        if (terminal.row >= TERM_ROWS) terminal.row = TERM_ROWS - 1;
        break;
    case 'C':
        terminal.col += amount;
        if (terminal.col >= TERM_COLS) terminal.col = TERM_COLS - 1;
        break;
    case 'D':
        terminal.col -= amount;
        if (terminal.col < 0) terminal.col = 0;
        break;
    case 'G':
        terminal.col = csi_arg(0, 1) - 1;
        if (terminal.col < 0) terminal.col = 0;
        if (terminal.col >= TERM_COLS) terminal.col = TERM_COLS - 1;
        break;
    case 'H':
    case 'f':
        terminal.row = csi_arg(0, 1) - 1;
        terminal.col = csi_arg(1, 1) - 1;
        if (terminal.row < 0) terminal.row = 0;
        if (terminal.row >= TERM_ROWS) terminal.row = TERM_ROWS - 1;
        if (terminal.col < 0) terminal.col = 0;
        if (terminal.col >= TERM_COLS) terminal.col = TERM_COLS - 1;
        break;
    case 'J':
        terminal_clear_screen(terminal.csi_argc ? terminal.csi_args[0] : 0);
        break;
    case 'K':
        terminal_clear_line(terminal.csi_argc ? terminal.csi_args[0] : 0);
        break;
    case 'm':
        break;
    case 's':
        terminal.saved_row = terminal.row;
        terminal.saved_col = terminal.col;
        break;
    case 'u':
        terminal.row = terminal.saved_row;
        terminal.col = terminal.saved_col;
        break;
    default:
        break;
    }
}

static void terminal_put_byte(unsigned char byte)
{
    if (terminal.escape_state == 1) {
        if (byte == '[') {
            terminal.escape_state = 2;
            memset(terminal.csi_args, 0, sizeof(terminal.csi_args));
            terminal.csi_argc = 1;
        } else if (byte == '7') {
            terminal.saved_row = terminal.row;
            terminal.saved_col = terminal.col;
            terminal.escape_state = 0;
        } else if (byte == '8') {
            terminal.row = terminal.saved_row;
            terminal.col = terminal.saved_col;
            terminal.escape_state = 0;
        } else {
            terminal.escape_state = 0;
        }
        return;
    }

    if (terminal.escape_state == 2) {
        if (byte >= '0' && byte <= '9') {
            int index = terminal.csi_argc - 1;
            terminal.csi_args[index] =
                terminal.csi_args[index] * 10 + byte - '0';
        } else if (byte == ';' && terminal.csi_argc < 8) {
            terminal.csi_argc++;
        } else if (byte == '?' || byte == '>') {
            return;
        } else {
            terminal_handle_csi((char)byte);
            terminal.escape_state = 0;
        }
        return;
    }

    switch (byte) {
    case 0x1b:
        terminal.escape_state = 1;
        return;
    case '\r':
        terminal.col = 0;
        return;
    case '\n':
        terminal.col = 0;
        terminal_newline();
        terminal.dirty = true;
        return;
    case '\b':
        if (terminal.col > 0)
            terminal.col--;
        terminal.dirty = true;
        return;
    case '\t':
        terminal.col = (terminal.col + 8) & ~7;
        if (terminal.col >= TERM_COLS) {
            terminal.col = 0;
            terminal_newline();
        }
        return;
    default:
        break;
    }

    if (byte < 32 || byte > 126)
        return;

    terminal.cells[terminal.row][terminal.col] = (char)byte;
    terminal.col++;
    if (terminal.col >= TERM_COLS) {
        terminal.col = 0;
        terminal_newline();
    }
    terminal.dirty = true;
}

static void terminal_render(void)
{
    if (!terminal.dirty || !terminal.output)
        return;

    char * out = terminal.rendered;
    for (int row = 0; row < TERM_ROWS; row++) {
        memcpy(out, terminal.cells[row], TERM_COLS);
        out += TERM_COLS;
        if (row != TERM_ROWS - 1)
            *out++ = '\n';
    }
    *out = '\0';

    lv_label_set_text(terminal.output, terminal.rendered);
    lv_obj_set_pos(terminal.cursor,
                   terminal.col * TERM_CELL_WIDTH,
                   terminal.row * TERM_CELL_HEIGHT);
    terminal.dirty = false;
}

static void terminal_poll(lv_timer_t * timer)
{
    (void)timer;

    if (terminal.master_fd >= 0) {
        char buffer[512];
        ssize_t count;
        do {
            count = read(terminal.master_fd, buffer, sizeof(buffer));
            if (count > 0) {
                for (ssize_t i = 0; i < count; i++)
                    terminal_put_byte((unsigned char)buffer[i]);
            }
        } while (count > 0);
    }

    if (terminal.shell_pid > 0) {
        int status;
        pid_t result = waitpid(terminal.shell_pid, &status, WNOHANG);
        if (result == terminal.shell_pid) {
            terminal.shell_pid = -1;
            lv_label_set_text(terminal.status, "Shell exited");
            lv_label_set_text(terminal.live, "OFFLINE");
            lv_obj_set_style_text_color(terminal.live,
                                        lv_color_hex(0xC97979), 0);
        }
    }

    terminal_render();
}

static void terminal_write(const char * text)
{
    if (terminal.master_fd < 0 || !text)
        return;
    write(terminal.master_fd, text, strlen(text));
}

static void terminal_set_focused(bool focused)
{
    if (!terminal.panel)
        return;

    lv_obj_set_style_border_color(
        terminal.panel,
        focused ? lv_color_hex(0x56B3A5) : lv_color_hex(0x303840), 0);
}

static void terminal_key(uint32_t key, void * user_data)
{
    (void)user_data;

    if (terminal.master_fd < 0)
        return;

    switch (key) {
    case LV_PORT_KEY_UP: terminal_write("\033[A"); break;
    case LV_PORT_KEY_DOWN: terminal_write("\033[B"); break;
    case LV_PORT_KEY_RIGHT: terminal_write("\033[C"); break;
    case LV_PORT_KEY_LEFT: terminal_write("\033[D"); break;
    case LV_PORT_KEY_DELETE: terminal_write("\033[3~"); break;
    case LV_PORT_KEY_HOME: terminal_write("\033[H"); break;
    case LV_PORT_KEY_END: terminal_write("\033[F"); break;
    case LV_PORT_KEY_PAGE_UP: terminal_write("\033[5~"); break;
    case LV_PORT_KEY_PAGE_DOWN: terminal_write("\033[6~"); break;
    default:
        if (key < 0x100) {
            char byte = (char)key;
            write(terminal.master_fd, &byte, 1);
        }
        break;
    }
}

static void terminal_focus_cb(lv_event_t * event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        lv_port_indev_set_key_handler(terminal_key, NULL);
        terminal_set_focused(true);
    }
}

static void terminal_close_shell(void)
{
    if (terminal.master_fd >= 0) {
        close(terminal.master_fd);
        terminal.master_fd = -1;
    }

    if (terminal.shell_pid > 0) {
        kill(terminal.shell_pid, SIGHUP);
        waitpid(terminal.shell_pid, NULL, WNOHANG);
        terminal.shell_pid = -1;
    }
}

static bool terminal_start_shell(void)
{
    struct winsize size = {
        .ws_row = TERM_ROWS,
        .ws_col = TERM_COLS,
    };

    terminal.shell_pid =
        forkpty(&terminal.master_fd, terminal.pty_name, NULL, &size);
    if (terminal.shell_pid == 0) {
        char * argv[] = {"mksh", "-i", NULL};
        char * envp[] = {
            "PATH=/bin:/usr/bin",
            "HOME=/",
            "SHELL=/bin/mksh",
            "TERM=xterm",
            "USER=root",
            NULL,
        };
        execve("/bin/mksh", argv, envp);
        _exit(127);
    }

    if (terminal.shell_pid < 0) {
        terminal.master_fd = -1;
        lv_label_set_text(terminal.status, "Unable to start /bin/mksh");
        lv_label_set_text(terminal.live, "OFFLINE");
        lv_obj_set_style_text_color(terminal.live,
                                    lv_color_hex(0xC97979), 0);
        return false;
    }

    int enabled = 1;
    ioctl(terminal.master_fd, FIONBIO, &enabled);
    lv_label_set_text_fmt(terminal.status, "mksh  |  %s  |  %d x %d",
                          terminal.pty_name, TERM_COLS, TERM_ROWS);
    lv_label_set_text(terminal.live, "CONNECTED");
    lv_obj_set_style_text_color(terminal.live, lv_color_hex(0x71C5B8), 0);
    return true;
}

lv_obj_t * desktop_terminal_create(lv_obj_t * parent)
{
    terminal_reset_cells();

    lv_obj_t * panel = lv_obj_create(parent);
    terminal.panel = panel;
    lv_obj_set_size(panel, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_pad_row(panel, 0, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x303840), 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x11161B), 0);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(panel, terminal_focus_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * header = lv_obj_create(panel);
    lv_obj_set_size(header, lv_pct(100), 40);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x242B31), 0);
    lv_obj_set_style_pad_hor(header, 14, 0);
    lv_obj_set_style_pad_ver(header, 0, 0);
    lv_obj_set_scrollable(header, false);

    lv_obj_t * title = lv_label_create(header);
    lv_label_set_text(title, ">_  Terminal");
    lv_obj_set_style_text_color(title, lv_color_hex(0xF2F5F7), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    terminal.live = lv_label_create(header);
    lv_label_set_text(terminal.live, "CONNECTING");
    lv_obj_set_style_text_color(terminal.live, lv_color_hex(0xD2A94A), 0);
    lv_obj_align(terminal.live, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t * viewport = lv_obj_create(panel);
    lv_obj_set_width(viewport, lv_pct(100));
    lv_obj_set_flex_grow(viewport, 1);
    lv_obj_set_style_radius(viewport, 0, 0);
    lv_obj_set_style_border_width(viewport, 0, 0);
    lv_obj_set_style_bg_color(viewport, lv_color_hex(0x11161B), 0);
    lv_obj_set_style_pad_left(viewport, 12, 0);
    lv_obj_set_style_pad_top(viewport, 10, 0);
    lv_obj_set_style_pad_right(viewport, 8, 0);
    lv_obj_set_style_pad_bottom(viewport, 8, 0);
    lv_obj_set_scrollbar_mode(viewport, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scrollable(viewport, false);

    terminal.output = lv_label_create(viewport);
    lv_obj_set_size(terminal.output,
                    TERM_COLS * TERM_CELL_WIDTH,
                    TERM_ROWS * TERM_CELL_HEIGHT);
    lv_label_set_long_mode(terminal.output, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_font(terminal.output, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(terminal.output, lv_color_hex(0xDCE4E8), 0);
    lv_obj_set_style_text_line_space(terminal.output, 0, 0);
    lv_obj_set_pos(terminal.output, 0, 0);

    terminal.cursor = lv_obj_create(viewport);
    lv_obj_set_size(terminal.cursor, TERM_CELL_WIDTH, TERM_CELL_HEIGHT);
    lv_obj_set_style_radius(terminal.cursor, 0, 0);
    lv_obj_set_style_border_width(terminal.cursor, 0, 0);
    lv_obj_set_style_bg_color(terminal.cursor, lv_color_hex(0x60B8AA), 0);
    lv_obj_set_style_bg_opa(terminal.cursor, LV_OPA_50, 0);
    lv_obj_set_scrollable(terminal.cursor, false);

    lv_obj_t * footer = lv_obj_create(panel);
    lv_obj_set_size(footer, lv_pct(100), 26);
    lv_obj_set_style_radius(footer, 0, 0);
    lv_obj_set_style_border_width(footer, 0, 0);
    lv_obj_set_style_bg_color(footer, lv_color_hex(0x1B2126), 0);
    lv_obj_set_style_pad_hor(footer, 12, 0);
    lv_obj_set_style_pad_ver(footer, 0, 0);
    lv_obj_set_scrollable(footer, false);

    terminal.status = lv_label_create(footer);
    lv_label_set_text(terminal.status, "Starting shell...");
    lv_obj_set_style_text_color(terminal.status, lv_color_hex(0x8F9DA5), 0);
    lv_obj_align(terminal.status, LV_ALIGN_LEFT_MID, 0, 0);

    terminal_render();
    terminal_start_shell();
    lv_port_indev_set_key_handler(terminal_key, NULL);
    terminal_set_focused(true);
    lv_timer_create(terminal_poll, 30, NULL);
    return panel;
}

void desktop_terminal_send_command(const char * command)
{
    if (!desktop_terminal_is_running())
        desktop_terminal_restart();

    terminal_write(command);
    terminal_write("\n");
}

void desktop_terminal_clear(void)
{
    terminal_reset_cells();
    terminal_render();
    terminal_write("\033[2J\033[H");
}

void desktop_terminal_restart(void)
{
    terminal_close_shell();
    terminal_reset_cells();
    terminal_render();
    lv_label_set_text(terminal.status, "Starting shell...");
    lv_label_set_text(terminal.live, "CONNECTING");
    lv_obj_set_style_text_color(terminal.live, lv_color_hex(0xD2A94A), 0);
    terminal_start_shell();
    lv_port_indev_set_key_handler(terminal_key, NULL);
    terminal_set_focused(true);
}

const char * desktop_terminal_get_pty_name(void)
{
    return terminal.pty_name[0] ? terminal.pty_name : "not connected";
}

bool desktop_terminal_is_running(void)
{
    return terminal.shell_pid > 0;
}

int desktop_terminal_get_columns(void)
{
    return TERM_COLS;
}

int desktop_terminal_get_rows(void)
{
    return TERM_ROWS;
}
