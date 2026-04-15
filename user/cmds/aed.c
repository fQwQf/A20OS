/*
 * A20OS — aed: A20 Editor (nano-like visual text editor)
 *
 * Key bindings:
 *   Arrow keys   Move cursor          Ctrl+S  Save
 *   Ctrl+Q       Quit                 Ctrl+O  Save as
 *   Ctrl+G       Help                 Ctrl+K  Cut line
 *   Ctrl+U       Paste line           Ctrl+D  Delete char
 *   Ctrl+A       Start of line        Ctrl+E  End of line
 *   Enter        New line             Tab     4 spaces
 *   Backspace    Delete before cursor
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>

#define MAX_LINES     4096
#define MAX_LINE_LEN  1024
#define TAB_STOP      4
#define CLIP_SIZE     (MAX_LINE_LEN + 1)

enum {
    KEY_UP = 1000,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_HOME,
    KEY_END,
    KEY_DEL,
};

/* ============================================================
 * Terminal helpers
 * ============================================================ */

static void term_clear(void)    { write(1, "\033[2J", 4); }
static void term_home(void)     { write(1, "\033[H", 3); }
static void term_clear_line(void){ write(1, "\033[K", 3); }
static void term_reverse(void)  { write(1, "\033[7m", 4); }
static void term_normal(void)   { write(1, "\033[0m", 4); }
static void term_show_cur(void) { write(1, "\033[?25h", 6); }
static void term_hide_cur(void) { write(1, "\033[?25l", 6); }

static void term_move(int row, int col) {
    char b[32];
    int n = snprintf(b, sizeof(b), "\033[%d;%dH", row, col);
    write(1, b, n);
}

static void term_color(int code) {
    char b[16];
    int n = snprintf(b, sizeof(b), "\033[%dm", code);
    write(1, b, n);
}

static void term_write(const char *s) {
    write(1, s, strlen(s));
}

static void term_get_size(int *rows, int *cols) {
    unsigned short ws[4] = {0, 0, 0, 0};
    int r = ioctl(0, TIOCGWINSZ, ws);
    if (r == 0 && ws[0] >= 8 && ws[1] >= 20) {
        *rows = ws[0];
        *cols = ws[1];
    } else {
        *rows = 24;
        *cols = 80;
    }
}

/* ============================================================
 * Editor state
 * ============================================================ */

typedef struct {
    char  **lines;
    int     num_lines;
    int     cap_lines;
    int     cx, cy;
    int     row_off, col_off;
    char    filename[256];
    int     dirty;
    int     quit_request;
    char    clipboard[CLIP_SIZE];
    int     clipboard_len;
    int     screen_rows, screen_cols;
    int     edit_rows;
} editor_t;

static editor_t g_ed;

/* ============================================================
 * Screen layout helpers (dynamic)
 * ============================================================ */

static int status_row(editor_t *ed) { return ed->screen_rows - 1; }
static int msg_row(editor_t *ed)    { return ed->screen_rows; }

static void ed_update_size(editor_t *ed) {
    term_get_size(&ed->screen_rows, &ed->screen_cols);
    ed->edit_rows = ed->screen_rows - 2;
    if (ed->edit_rows < 3) ed->edit_rows = 3;
}

/* ============================================================
 * Line management
 * ============================================================ */

static void ed_init(editor_t *ed) {
    memset(ed, 0, sizeof(*ed));
    ed->cap_lines = 128;
    ed->lines = (char **)malloc(sizeof(char *) * ed->cap_lines);
    ed->lines[0] = strdup("");
    ed->num_lines = 1;
    ed->clipboard[0] = '\0';
    ed->clipboard_len = 0;
    ed_update_size(ed);
}

static void ed_ensure_cap(editor_t *ed, int needed) {
    if (needed <= ed->cap_lines) return;
    int nc = ed->cap_lines * 2;
    if (nc < needed) nc = needed;
    ed->lines = (char **)realloc(ed->lines, sizeof(char *) * nc);
    ed->cap_lines = nc;
}

static void ed_insert_line(editor_t *ed, int at, const char *s) {
    ed_ensure_cap(ed, ed->num_lines + 1);
    for (int i = ed->num_lines; i > at; i--)
        ed->lines[i] = ed->lines[i - 1];
    ed->lines[at] = strdup(s);
    ed->num_lines++;
    ed->dirty = 1;
}

static void ed_delete_line(editor_t *ed, int at) {
    if (at < 0 || at >= ed->num_lines) return;
    free(ed->lines[at]);
    for (int i = at; i < ed->num_lines - 1; i++)
        ed->lines[i] = ed->lines[i + 1];
    ed->num_lines--;
    if (ed->num_lines == 0) {
        ed->lines[0] = strdup("");
        ed->num_lines = 1;
    }
    ed->dirty = 1;
}

/* ============================================================
 * File I/O
 * ============================================================ */

static int ed_load_file(editor_t *ed, const char *path) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        strncpy(ed->filename, path, sizeof(ed->filename) - 1);
        return 0;
    }

    off_t fsize = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (fsize <= 0) {
        close(fd);
        strncpy(ed->filename, path, sizeof(ed->filename) - 1);
        return 0;
    }

    char *buf = (char *)malloc(fsize + 1);
    if (!buf) { close(fd); return -1; }

    ssize_t nread = 0;
    while (nread < fsize) {
        ssize_t r = read(fd, buf + nread, fsize - nread);
        if (r <= 0) break;
        nread += r;
    }
    buf[nread] = '\0';
    close(fd);

    free(ed->lines[0]);
    ed->num_lines = 0;

    char *p = buf;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (nl) {
            *nl = '\0';
            ed_ensure_cap(ed, ed->num_lines + 1);
            ed->lines[ed->num_lines++] = strdup(p);
            p = nl + 1;
        } else {
            ed_ensure_cap(ed, ed->num_lines + 1);
            ed->lines[ed->num_lines++] = strdup(p);
            break;
        }
    }
    if (ed->num_lines == 0) {
        ed->lines[0] = strdup("");
        ed->num_lines = 1;
    }

    free(buf);
    strncpy(ed->filename, path, sizeof(ed->filename) - 1);
    ed->dirty = 0;
    return 0;
}

static int ed_save_file(editor_t *ed, const char *path) {
    if (!path || !path[0]) return -1;

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;

    for (int i = 0; i < ed->num_lines; i++) {
        const char *line = ed->lines[i];
        size_t len = strlen(line);
        if (len > 0) write(fd, line, len);
        write(fd, "\n", 1);
    }

    close(fd);
    strncpy(ed->filename, path, sizeof(ed->filename) - 1);
    ed->dirty = 0;
    return 0;
}

/* ============================================================
 * Input prompt
 * ============================================================ */

static int prompt_input(editor_t *ed, const char *msg, char *out, int outsz) {
    int mr = msg_row(ed);
    term_move(mr, 1);
    term_clear_line();
    term_reverse();
    term_write(msg);
    term_normal();

    int pos = 0;
    while (1) {
        int c = getchar();
        if (c == '\n' || c == '\r') {
            out[pos] = '\0';
            return pos;
        }
        if (c == 3) { out[0] = '\0'; return -1; }
        if (c == 127 || c == 8) {
            if (pos > 0) { pos--; write(1, "\b \b", 3); }
            continue;
        }
        if (c == 27) {
            int c2 = getchar();
            if (c2 == '[') getchar();
            out[0] = '\0';
            return -1;
        }
        if (c >= 32 && c < 127 && pos < outsz - 1) {
            out[pos++] = (char)c;
            write(1, &c, 1);
        }
    }
}

/* ============================================================
 * Drawing
 * ============================================================ */

static void ed_draw(editor_t *ed) {
    int sc = ed->screen_cols;
    int er = ed->edit_rows;
    int sr = status_row(ed);
    int mr = msg_row(ed);
    char buf[256];

    term_hide_cur();

    /* Text area */
    for (int row = 0; row < er; row++) {
        int file_row = row + ed->row_off;
        term_move(row + 1, 1);
        term_clear_line();

        if (file_row < ed->num_lines) {
            const char *line = ed->lines[file_row];
            int len = (int)strlen(line);
            int start = ed->col_off;
            int avail = sc - 1;
            if (start < len) {
                int written = 0;
                for (int i = start; i < len && written < avail; i++) {
                    char ch = line[i];
                    if (ch == '\t') {
                        int sp = TAB_STOP - (written % TAB_STOP);
                        for (int s = 0; s < sp && written < avail; s++) {
                            write(1, " ", 1);
                            written++;
                        }
                    } else if (ch >= 32 && ch < 127) {
                        write(1, &ch, 1);
                        written++;
                    } else {
                        write(1, "\xc2\xb7", 2);
                        written++;
                    }
                }
            }
        } else {
            term_color(34);
            write(1, "~", 1);
            term_normal();
        }
    }

    /* Status bar: filename + position */
    term_move(sr, 1);
    term_clear_line();
    term_reverse();

    const char *fname = ed->filename[0] ? ed->filename : "[New File]";
    int pct = ed->num_lines > 0 ? (ed->cy + 1) * 100 / ed->num_lines : 100;
    if (pct > 100) pct = 100;
    int n = snprintf(buf, sizeof(buf), " aed  %s%s  |  Ln %d, Col %d  |  %d%% ",
                     fname, ed->dirty ? " [+]" : "",
                     ed->cy + 1, ed->cx + 1, pct);
    int padding = sc - n;
    if (padding < 0) { if (n > sc) n = sc; padding = 0; }
    write(1, buf, n);
    for (int i = 0; i < padding; i++) write(1, " ", 1);
    term_normal();

    /* Shortcut bar: bottom row */
    term_move(mr, 1);
    term_clear_line();
    term_reverse();
    term_color(32);
    n = snprintf(buf, sizeof(buf),
        " ^S Save  ^O SaveAs  ^Q Quit  ^G Help  ^K Cut  ^U Paste  ^D Del ");
    if (n > sc) n = sc;
    write(1, buf, n);
    for (int i = n; i < sc; i++) write(1, " ", 1);
    term_normal();

    /* Position cursor in text area */
    int screen_row = ed->cy - ed->row_off + 1;
    int screen_col = ed->cx - ed->col_off + 1;
    if (screen_row < 1) screen_row = 1;
    if (screen_row > er) screen_row = er;
    if (screen_col < 1) screen_col = 1;
    if (screen_col > sc) screen_col = sc;

    term_move(screen_row, screen_col);
    term_show_cur();
}

static void ed_show_msg(editor_t *ed, const char *msg, int color) {
    int mr = msg_row(ed);
    term_move(mr, 1);
    term_clear_line();
    term_reverse();
    term_color(color);
    term_write(msg);
    term_normal();
}

/* ============================================================
 * Help screen
 * ============================================================ */

static void ed_show_help(editor_t *ed) {
    (void)ed;
    term_clear();
    term_home();
    term_reverse();
    term_write("  aed — A20 Editor  |  Press any key to return\n");
    term_normal();
    term_write("\n");
    term_color(36);
    term_write("  Navigation:\n");
    term_normal();
    term_write("  Arrow keys       Move cursor\n");
    term_write("  Ctrl+A           Start of line\n");
    term_write("  Ctrl+E           End of line\n");
    term_write("  Home / End       Start / end of line\n");
    term_write("\n");
    term_color(36);
    term_write("  Editing:\n");
    term_normal();
    term_write("  Enter            New line\n");
    term_write("  Backspace        Delete char before cursor\n");
    term_write("  Ctrl+D           Delete char at cursor\n");
    term_write("  Tab              Insert 4 spaces\n");
    term_write("  Ctrl+K           Cut current line\n");
    term_write("  Ctrl+U           Paste line\n");
    term_write("\n");
    term_color(36);
    term_write("  File:\n");
    term_normal();
    term_write("  Ctrl+S           Save file\n");
    term_write("  Ctrl+O           Save as\n");
    term_write("  Ctrl+Q           Quit (twice if unsaved)\n");
    term_write("\n");
    term_color(32);
    term_write("  aed — A20OS Text Editor\n");
    term_normal();

    getchar();
    int c = getchar();
    if (c == '[') getchar();
}

/* ============================================================
 * Cursor / scroll
 * ============================================================ */

static void ed_adjust_scroll(editor_t *ed) {
    int er = ed->edit_rows;
    int sc = ed->screen_cols;

    if (ed->cy < ed->row_off)
        ed->row_off = ed->cy;
    if (ed->cy >= ed->row_off + er)
        ed->row_off = ed->cy - er + 1;

    if (ed->cx < ed->col_off)
        ed->col_off = ed->cx;
    if (ed->cx >= ed->col_off + sc - 1)
        ed->col_off = ed->cx - sc + 2;
}

static void ed_clamp_cursor(editor_t *ed) {
    if (ed->cy < 0) ed->cy = 0;
    if (ed->cy >= ed->num_lines) ed->cy = ed->num_lines - 1;
    int line_len = (int)strlen(ed->lines[ed->cy]);
    if (ed->cx > line_len) ed->cx = line_len;
    if (ed->cx < 0) ed->cx = 0;
}

/* ============================================================
 * Editor operations
 * ============================================================ */

static void ed_insert_char(editor_t *ed, int ch) {
    char *line = ed->lines[ed->cy];
    int len = (int)strlen(line);
    if (len >= MAX_LINE_LEN - 2) return;

    char *nl = (char *)malloc(len + 2);
    memcpy(nl, line, ed->cx);
    nl[ed->cx] = (char)ch;
    memcpy(nl + ed->cx + 1, line + ed->cx, len - ed->cx);
    nl[len + 1] = '\0';

    free(ed->lines[ed->cy]);
    ed->lines[ed->cy] = nl;
    ed->cx++;
    ed->dirty = 1;
}

static void ed_insert_tab(editor_t *ed) {
    for (int i = 0; i < TAB_STOP; i++)
        ed_insert_char(ed, ' ');
}

static void ed_delete_char(editor_t *ed) {
    if (ed->cx > 0) {
        char *line = ed->lines[ed->cy];
        int len = (int)strlen(line);
        memmove(line + ed->cx - 1, line + ed->cx, len - ed->cx + 1);
        ed->cx--;
        ed->dirty = 1;
    } else if (ed->cy > 0) {
        char *prev = ed->lines[ed->cy - 1];
        char *cur  = ed->lines[ed->cy];
        int pl = (int)strlen(prev);
        int cl = (int)strlen(cur);

        char *m = (char *)malloc(pl + cl + 1);
        memcpy(m, prev, pl);
        memcpy(m + pl, cur, cl);
        m[pl + cl] = '\0';

        free(prev);
        free(cur);
        ed->lines[ed->cy - 1] = m;
        for (int i = ed->cy; i < ed->num_lines - 1; i++)
            ed->lines[i] = ed->lines[i + 1];
        ed->num_lines--;
        if (ed->num_lines == 0) {
            ed->lines[0] = strdup("");
            ed->num_lines = 1;
        }
        ed->cx = pl;
        ed->cy--;
        ed->dirty = 1;
    }
}

static void ed_delete_forward(editor_t *ed) {
    char *line = ed->lines[ed->cy];
    int len = (int)strlen(line);

    if (ed->cx < len) {
        memmove(line + ed->cx, line + ed->cx + 1, len - ed->cx);
        ed->dirty = 1;
    } else if (ed->cy < ed->num_lines - 1) {
        char *next = ed->lines[ed->cy + 1];
        int nl = (int)strlen(next);

        char *m = (char *)malloc(len + nl + 1);
        memcpy(m, line, len);
        memcpy(m + len, next, nl);
        m[len + nl] = '\0';

        free(line);
        free(next);
        ed->lines[ed->cy] = m;
        for (int i = ed->cy + 1; i < ed->num_lines - 1; i++)
            ed->lines[i] = ed->lines[i + 1];
        ed->num_lines--;
        ed->dirty = 1;
    }
}

static void ed_insert_newline(editor_t *ed) {
    char *line = ed->lines[ed->cy];
    int len = (int)strlen(line);

    char *left  = (char *)malloc(ed->cx + 1);
    char *right = (char *)malloc(len - ed->cx + 1);
    memcpy(left, line, ed->cx);
    left[ed->cx] = '\0';
    memcpy(right, line + ed->cx, len - ed->cx);
    right[len - ed->cx] = '\0';

    free(line);
    ed->lines[ed->cy] = left;
    ed_insert_line(ed, ed->cy + 1, right);
    free(ed->lines[ed->cy + 1]);
    ed->lines[ed->cy + 1] = right;

    ed->cx = 0;
    ed->cy++;
    ed->dirty = 1;
}

static void ed_cut_line(editor_t *ed) {
    if (ed->num_lines <= 1 && strlen(ed->lines[0]) == 0) return;

    char *line = ed->lines[ed->cy];
    int len = (int)strlen(line);
    if (len >= CLIP_SIZE) len = CLIP_SIZE - 1;
    memcpy(ed->clipboard, line, len);
    ed->clipboard[len] = '\0';
    ed->clipboard_len = len;

    ed_delete_line(ed, ed->cy);
    ed_clamp_cursor(ed);
}

static void ed_paste_line(editor_t *ed) {
    if (ed->clipboard_len == 0) return;
    ed_insert_line(ed, ed->cy, ed->clipboard);
    ed->dirty = 1;
}

/* ============================================================
 * Input handling
 * ============================================================ */

static int ed_read_key(void) {
    int c = getchar();
    if (c == 27) {
        int c2 = getchar();
        if (c2 == '[') {
            int c3 = getchar();
            switch (c3) {
                case 'A': return KEY_UP;
                case 'B': return KEY_DOWN;
                case 'C': return KEY_RIGHT;
                case 'D': return KEY_LEFT;
                case 'H': return KEY_HOME;
                case 'F': return KEY_END;
                case '1': { int c4 = getchar(); return (c4 == '~') ? KEY_HOME : 27; }
                case '3': { int c4 = getchar(); return (c4 == '~') ? KEY_DEL  : 27; }
                case '4': { int c4 = getchar(); return (c4 == '~') ? KEY_END  : 27; }
                default: return 27;
            }
        }
        return 27;
    }
    return c;
}

/* ============================================================
 * Main loop
 * ============================================================ */

static void do_quit(void) {
    term_clear();
    term_home();
    term_show_cur();
    _exit(0);
}

int main(int argc, char *argv[]) {
    ed_init(&g_ed);

    if (argc > 1) {
        if (ed_load_file(&g_ed, argv[1]) < 0) {
            term_write("aed: cannot open ");
            term_write(argv[1]);
            term_write("\n");
            _exit(1);
        }
    }

    term_clear();
    ed_draw(&g_ed);

    while (1) {
        int c = ed_read_key();

        if (c >= 32 && c < 127) {
            g_ed.quit_request = 0;
            ed_insert_char(&g_ed, c);
            ed_adjust_scroll(&g_ed);
            ed_draw(&g_ed);
            continue;
        }

        switch (c) {
        case '\n': case '\r':
            ed_insert_newline(&g_ed);
            break;

        case 8: case 127:
            ed_delete_char(&g_ed);
            break;

        case 9:
            ed_insert_tab(&g_ed);
            break;

        case 19: /* Ctrl+S */
            if (g_ed.filename[0]) {
                if (ed_save_file(&g_ed, g_ed.filename) == 0) {
                    ed_draw(&g_ed);
                    ed_show_msg(&g_ed, " Saved.", 36);
                } else {
                    ed_draw(&g_ed);
                    ed_show_msg(&g_ed, " Error saving file!", 31);
                }
            } else {
                char name[256];
                ed_draw(&g_ed);
                int r = prompt_input(&g_ed, " Save as: ", name, sizeof(name));
                if (r > 0) {
                    if (ed_save_file(&g_ed, name) == 0) {
                        ed_draw(&g_ed);
                        ed_show_msg(&g_ed, " Saved.", 36);
                    } else {
                        ed_draw(&g_ed);
                        ed_show_msg(&g_ed, " Error saving file!", 31);
                    }
                } else {
                    ed_draw(&g_ed);
                }
            }
            continue;

        case 17: /* Ctrl+Q */
            if (!g_ed.dirty) do_quit();
            if (g_ed.quit_request) do_quit();
            g_ed.quit_request = 1;
            ed_draw(&g_ed);
            ed_show_msg(&g_ed, " Unsaved changes! ^Q again to force quit.", 31);
            continue;

        case 7: /* Ctrl+G */
            g_ed.quit_request = 0;
            ed_show_help(&g_ed);
            break;

        case 15: /* Ctrl+O */
            {
                char name[256];
                ed_draw(&g_ed);
                int r = prompt_input(&g_ed, " Save as: ", name, sizeof(name));
                if (r > 0) {
                    if (ed_save_file(&g_ed, name) == 0) {
                        ed_draw(&g_ed);
                        ed_show_msg(&g_ed, " Saved.", 36);
                    } else {
                        ed_draw(&g_ed);
                        ed_show_msg(&g_ed, " Error saving file!", 31);
                    }
                } else {
                    ed_draw(&g_ed);
                }
            }
            continue;

        case 11: ed_cut_line(&g_ed); break;
        case 21: ed_paste_line(&g_ed); break;
        case 4:  ed_delete_forward(&g_ed); break;
        case 1:  g_ed.cx = 0; break;
        case 5:  g_ed.cx = (int)strlen(g_ed.lines[g_ed.cy]); break;

        case KEY_UP:
            if (g_ed.cy > 0) g_ed.cy--;
            break;
        case KEY_DOWN:
            if (g_ed.cy < g_ed.num_lines - 1) g_ed.cy++;
            break;
        case KEY_LEFT:
            if (g_ed.cx > 0) g_ed.cx--;
            else if (g_ed.cy > 0) { g_ed.cy--; g_ed.cx = (int)strlen(g_ed.lines[g_ed.cy]); }
            break;
        case KEY_RIGHT:
            if (g_ed.cx < (int)strlen(g_ed.lines[g_ed.cy])) g_ed.cx++;
            else if (g_ed.cy < g_ed.num_lines - 1) { g_ed.cy++; g_ed.cx = 0; }
            break;
        case KEY_HOME: g_ed.cx = 0; break;
        case KEY_END:  g_ed.cx = (int)strlen(g_ed.lines[g_ed.cy]); break;
        case KEY_DEL:  ed_delete_forward(&g_ed); break;

        default: g_ed.quit_request = 0; continue;
        }

        ed_clamp_cursor(&g_ed);
        ed_adjust_scroll(&g_ed);
        ed_draw(&g_ed);
    }

    return 0;
}
