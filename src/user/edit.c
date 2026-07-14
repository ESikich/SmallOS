#include "term_keys.h"
#include "user_lib.h"
#include "editor_model.h"

#define EDIT_LINE_MAX EDITOR_LINE_MAX
#define EDIT_DEFAULT_ROWS 25u
#define EDIT_DEFAULT_COLS 80u

enum {
    KEY_NONE = 0,
    KEY_CHAR = 256,
    KEY_ESC,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_HOME,
    KEY_END,
    KEY_DELETE,
    KEY_PAGEUP,
    KEY_PAGEDOWN,
    KEY_F1,
    KEY_F2,
    KEY_F3,
};

typedef struct {
    int type;
    char ch;
} key_t;

typedef editor_model_t edit_buffer_t;

typedef struct {
    uint32_t cy;
    uint32_t cx;
    uint32_t top;
    uint32_t hscroll;
    uint32_t rows;
    uint32_t cols;
    uint32_t text_rows;
    uint32_t text_cols;
    int status_ticks;
    char status[80];
} edit_view_t;

typedef struct {
    int active;
    uint32_t index;
    uint32_t replace_end;
} input_mode_t;

static int streq(const char* a, const char* b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static char* skip_spaces(char* s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

static int parse_uint(char** ps, uint32_t* out) {
    char* s = skip_spaces(*ps);
    uint32_t value = 0;
    int any = 0;

    while (*s >= '0' && *s <= '9') {
        value = value * 10u + (uint32_t)(*s - '0');
        s++;
        any = 1;
    }

    if (!any) return 0;
    *out = value;
    *ps = s;
    return 1;
}

static void copy_status(edit_view_t* view, const char* msg) {
    uint32_t i = 0;
    while (msg[i] && i < sizeof(view->status) - 1u) {
        view->status[i] = msg[i];
        i++;
    }
    view->status[i] = '\0';
    view->status_ticks = 2;
}

static void free_buffer(edit_buffer_t* buf) {
    editor_model_destroy(buf);
}

static int insert_line(edit_buffer_t* buf, uint32_t index, const char* text) {
    return editor_model_insert_line(buf, index, text);
}

static int delete_range(edit_buffer_t* buf, uint32_t first, uint32_t last) {
    if (first == 0 || last < first || first > buf->count) return 0;
    return editor_model_delete_lines(buf, first - 1u, last - 1u);
}

static void clear_buffer(edit_buffer_t* buf) {
    editor_model_clear(buf);
}

static uint32_t line_len(edit_buffer_t* buf, uint32_t y) {
    return editor_model_line_length(buf, y);
}

static int ensure_line(edit_buffer_t* buf, uint32_t y) {
    return editor_model_ensure_line(buf, y);
}

static void term_write(const char* s) {
    sys_write(s, str_len(s));
}

static void term_move(uint32_t row, uint32_t col) {
    char seq[24];
    char* p = seq;
    *p++ = 27;
    *p++ = '[';

    uint32_t values[2];
    values[0] = row + 1u;
    values[1] = col + 1u;
    for (int v = 0; v < 2; v++) {
        char digits[12];
        int n = 0;
        uint32_t value = values[v];
        if (v) *p++ = ';';
        if (value == 0) {
            *p++ = '0';
        } else {
            while (value) {
                digits[n++] = (char)('0' + value % 10u);
                value /= 10u;
            }
            while (n) {
                *p++ = digits[--n];
            }
        }
    }
    *p++ = 'H';
    *p = '\0';
    term_write(seq);
}

static void term_clear_line(void) {
    term_write("\x1b[K");
}

static uint32_t draw_limited(const char* s, uint32_t max) {
    uint32_t i = 0;
    while (s && s[i] && i < max) {
        sys_putc(s[i]);
        i++;
    }
    return i;
}

static void draw_padded(const char* s, uint32_t max) {
    uint32_t i = draw_limited(s, max);
    while (i < max) {
        sys_putc(' ');
        i++;
    }
}

static void init_view_dimensions(edit_view_t* view) {
    uint32_t rows = EDIT_DEFAULT_ROWS;
    uint32_t cols = EDIT_DEFAULT_COLS;

    if (sys_terminal_size(&rows, &cols) < 0 || rows < 3u || cols < 2u) {
        rows = EDIT_DEFAULT_ROWS;
        cols = EDIT_DEFAULT_COLS;
    }

    view->rows = rows;
    view->cols = cols;
    view->text_rows = rows - 2u;
    view->text_cols = cols - 1u;
}

static int update_view(edit_buffer_t* buf, edit_view_t* view) {
    uint32_t old_top = view->top;
    uint32_t old_hscroll = view->hscroll;
    uint32_t len = line_len(buf, view->cy);

    if (view->cx > len) view->cx = len;
    if (view->cy < view->top) view->top = view->cy;
    if (view->cy >= view->top + view->text_rows) {
        view->top = view->cy - view->text_rows + 1u;
    }
    if (view->cx < view->hscroll) view->hscroll = view->cx;
    if (view->cx >= view->hscroll + view->text_cols) {
        view->hscroll = view->cx - view->text_cols + 1u;
    }

    return old_top != view->top || old_hscroll != view->hscroll;
}

static void render_title(edit_buffer_t* buf, edit_view_t* view) {
    const char* title = " SmallOS EDIT  ";
    const char* suffix = " F2 Save  F3 Exit";
    uint32_t max = view->text_cols;
    uint32_t suffix_len = str_len(suffix);
    uint32_t used = 0;
    uint32_t path_max = 0;

    term_move(0, 0);
    used += draw_limited(title, max - used);
    if (max > used + suffix_len) {
        path_max = max - used - suffix_len;
    }
    used += draw_limited(buf->path, path_max);
    while (used < max && used + suffix_len < max) {
        sys_putc(' ');
        used++;
    }
    draw_limited(suffix, max - used);
    term_clear_line();
}

static void render_text_line(edit_buffer_t* buf, edit_view_t* view, uint32_t row) {
    uint32_t y = view->top + row;

    term_move(row + 1u, 0);
    if (y < buf->count) {
        char* line = buf->lines[y];
        uint32_t x = view->hscroll;
        uint32_t printed = 0;
        while (line[x] && printed < view->text_cols) {
            char c = line[x++];
            sys_putc(c == '\t' ? ' ' : c);
            printed++;
        }
    }
    term_clear_line();
}

static void render_status(edit_buffer_t* buf, edit_view_t* view) {
    term_move(view->rows - 1u, 0);
    if (view->status_ticks > 0) {
        draw_padded(view->status, view->text_cols);
        term_clear_line();
        view->status_ticks--;
    } else {
        term_write(buf->dirty ? "* " : "  ");
        term_write("Ln ");
        u_put_uint(view->cy + 1u);
        term_write(" Col ");
        u_put_uint(view->cx + 1u);
        term_write("    F2 Save  F3 Exit");
        term_clear_line();
    }
}

static void place_cursor(edit_view_t* view) {
    term_move((view->cy - view->top) + 1u, view->cx - view->hscroll);
}

static void render(edit_buffer_t* buf, edit_view_t* view, int full, int line_dirty) {
    full = update_view(buf, view) || full;
    term_write("\x1b[?25l");

    if (full) {
        render_title(buf, view);
        for (uint32_t row = 0; row < view->text_rows; row++) {
            render_text_line(buf, view, row);
        }
    } else if (line_dirty && view->cy >= view->top &&
               view->cy < view->top + view->text_rows) {
        render_text_line(buf, view, view->cy - view->top);
    }

    render_status(buf, view);
    place_cursor(view);
    term_write("\x1b[?25h");
}

static key_t read_key(void) {
    key_t key;
    int c;

    key.type = KEY_NONE;
    key.ch = 0;

    c = term_key_read(1);
    if (c > 0 && c < 256) {
        key.type = KEY_CHAR;
        key.ch = (char)c;
        return key;
    }

    switch (c) {
        case TERM_KEY_ENTER:
            key.type = KEY_CHAR;
            key.ch = '\n';
            break;
        case TERM_KEY_BACKSPACE:
            key.type = KEY_CHAR;
            key.ch = '\b';
            break;
        case TERM_KEY_TAB:
            key.type = KEY_CHAR;
            key.ch = '\t';
            break;
        case TERM_KEY_ESC: key.type = KEY_ESC; break;
        case TERM_KEY_UP: key.type = KEY_UP; break;
        case TERM_KEY_DOWN: key.type = KEY_DOWN; break;
        case TERM_KEY_LEFT: key.type = KEY_LEFT; break;
        case TERM_KEY_RIGHT: key.type = KEY_RIGHT; break;
        case TERM_KEY_HOME: key.type = KEY_HOME; break;
        case TERM_KEY_END: key.type = KEY_END; break;
        case TERM_KEY_DELETE: key.type = KEY_DELETE; break;
        case TERM_KEY_PAGE_UP: key.type = KEY_PAGEUP; break;
        case TERM_KEY_PAGE_DOWN: key.type = KEY_PAGEDOWN; break;
        case TERM_KEY_F1: key.type = KEY_F1; break;
        case TERM_KEY_F2: key.type = KEY_F2; break;
        case TERM_KEY_F3: key.type = KEY_F3; break;
        default: break;
    }
    return key;
}

static int save_file(edit_buffer_t* buf) {
    return editor_model_save(buf);
}

static int load_file(edit_buffer_t* buf) {
    return editor_model_load(buf);
}

static int line_insert_char(edit_buffer_t* buf, edit_view_t* view, char c) {
    uint32_t len;
    if (!ensure_line(buf, view->cy)) return 0;
    len = line_len(buf, view->cy);
    if (view->cx > len) view->cx = len;
    if (!editor_model_insert_char(buf, view->cy, view->cx, c)) return 0;
    view->cx++;
    return 1;
}

static int split_line(edit_buffer_t* buf, edit_view_t* view) {
    if (!editor_model_split_line(buf, view->cy, view->cx)) return 0;
    view->cy++;
    view->cx = 0;
    return 1;
}

static void backspace_key(edit_buffer_t* buf, edit_view_t* view) {
    (void)editor_model_backspace(buf, &view->cy, &view->cx);
}

static void delete_key(edit_buffer_t* buf, edit_view_t* view) {
    (void)editor_model_delete(buf, view->cy, view->cx);
}

static void move_vertical(edit_buffer_t* buf, edit_view_t* view, int delta) {
    if (delta < 0) {
        uint32_t amount = (uint32_t)(-delta);
        view->cy = amount > view->cy ? 0 : view->cy - amount;
    } else {
        view->cy += (uint32_t)delta;
        if (buf->count == 0) {
            view->cy = 0;
        } else if (view->cy >= buf->count) {
            view->cy = buf->count - 1u;
        }
    }
    if (view->cx > line_len(buf, view->cy)) view->cx = line_len(buf, view->cy);
}

static void show_help(edit_view_t* view) {
    copy_status(view, "F2 Save  F3 Exit");
}

static int confirm_quit(edit_buffer_t* buf, edit_view_t* view) {
    if (!buf->dirty) return 1;

    copy_status(view, "Unsaved changes: F2 save and quit, F3 discard, Esc cancel");
    render(buf, view, 0, 0);
    for (;;) {
        key_t key = read_key();
        if (key.type == KEY_F2) {
            if (save_file(buf)) return 1;
            copy_status(view, "Save failed");
            render(buf, view, 0, 0);
        } else if (key.type == KEY_F3) {
            return 1;
        } else if (key.type == KEY_ESC) {
            copy_status(view, "Quit canceled");
            return 0;
        }
    }
}

static int interactive(edit_buffer_t* buf) {
    edit_view_t view;
    view.cy = 0;
    view.cx = 0;
    view.top = 0;
    view.hscroll = 0;
    init_view_dimensions(&view);
    view.status_ticks = 0;
    view.status[0] = '\0';

    term_write("\x1b[2J");
    render(buf, &view, 1, 0);

    for (;;) {
        key_t key = read_key();
        int full = 0;
        int line_dirty = 0;

        if (key.type == KEY_F1) {
            show_help(&view);
        } else if (key.type == KEY_F2) {
            copy_status(&view, save_file(buf) ? "Saved" : "Save failed");
        } else if (key.type == KEY_F3 || key.type == KEY_ESC) {
            if (confirm_quit(buf, &view)) break;
        } else if (key.type == KEY_UP) {
            move_vertical(buf, &view, -1);
        } else if (key.type == KEY_DOWN) {
            move_vertical(buf, &view, 1);
        } else if (key.type == KEY_LEFT) {
            if (view.cx > 0) {
                view.cx--;
            } else if (view.cy > 0) {
                view.cy--;
                view.cx = line_len(buf, view.cy);
            }
        } else if (key.type == KEY_RIGHT) {
            uint32_t len = line_len(buf, view.cy);
            if (view.cx < len) {
                view.cx++;
            } else if (view.cy + 1u < buf->count) {
                view.cy++;
                view.cx = 0;
            }
        } else if (key.type == KEY_HOME) {
            view.cx = 0;
        } else if (key.type == KEY_END) {
            view.cx = line_len(buf, view.cy);
        } else if (key.type == KEY_PAGEUP) {
            move_vertical(buf, &view, -(int)view.text_rows);
        } else if (key.type == KEY_PAGEDOWN) {
            move_vertical(buf, &view, (int)view.text_rows);
        } else if (key.type == KEY_DELETE) {
            uint32_t old_count = buf->count;
            delete_key(buf, &view);
            line_dirty = 1;
            if (buf->count != old_count) full = 1;
        } else if (key.type == KEY_CHAR) {
            if (key.ch == '\b') {
                uint32_t old_count = buf->count;
                backspace_key(buf, &view);
                line_dirty = 1;
                if (buf->count != old_count) full = 1;
            } else if (key.ch == '\n' || key.ch == '\r') {
                if (!split_line(buf, &view)) {
                    copy_status(&view, "Cannot split line");
                } else {
                    full = 1;
                }
            } else if (key.ch == '\t') {
                for (int i = 0; i < 4; i++) {
                    if (!line_insert_char(buf, &view, ' ')) break;
                }
                line_dirty = 1;
            } else if ((unsigned char)key.ch >= 32u) {
                if (!line_insert_char(buf, &view, key.ch)) copy_status(&view, "Line too long");
                line_dirty = 1;
            }
        }

        render(buf, &view, full, line_dirty);
    }

    term_write("\x1b[2J\x1b[H");
    return 0;
}

static void print_range(edit_buffer_t* buf, uint32_t first, uint32_t last) {
    if (buf->count == 0) {
        u_puts("edit: empty\n");
        return;
    }
    if (first == 0) first = 1;
    if (last == 0 || last > buf->count) last = buf->count;
    if (first > last || first > buf->count) {
        u_puts("edit: bad range\n");
        return;
    }

    for (uint32_t i = first; i <= last; i++) {
        u_put_uint(i);
        u_puts(": ");
        u_puts(buf->lines[i - 1u]);
        u_putc('\n');
    }
}

static int handle_input_line(edit_buffer_t* buf, input_mode_t* mode, const char* line) {
    if (streq(line, ".")) {
        if (mode->replace_end) {
            delete_range(buf, mode->index + 1u, mode->replace_end);
        }
        mode->active = 0;
        mode->replace_end = 0;
        return 0;
    }

    if (mode->replace_end) {
        delete_range(buf, mode->index + 1u, mode->replace_end);
        mode->replace_end = 0;
    }

    if (!insert_line(buf, mode->index, line)) {
        mode->active = 0;
        return 1;
    }
    mode->index++;
    return 0;
}

static int batch_command(edit_buffer_t* buf, input_mode_t* mode, char* line) {
    char* s;
    uint32_t first = 0;
    uint32_t last = 0;

    if (mode->active) return handle_input_line(buf, mode, line);

    s = skip_spaces(line);
    if (*s == '\0') return 0;
    if (streq(s, "q") || streq(s, "q!") || streq(s, "wq")) {
        if (s[0] == 'w' && !save_file(buf)) return 1;
        return 2;
    }
    if (streq(s, "w")) return save_file(buf) ? 0 : 1;
    if (streq(s, "c")) {
        clear_buffer(buf);
        return 0;
    }
    if (*s == 'p') {
        s++;
        if (!parse_uint(&s, &first)) {
            first = 1;
            last = buf->count;
        } else if (!parse_uint(&s, &last)) {
            last = first;
        }
        print_range(buf, first, last);
        return 0;
    }
    if (*s == 'd') {
        s++;
        if (!parse_uint(&s, &first)) return 1;
        if (!parse_uint(&s, &last)) last = first;
        return delete_range(buf, first, last) ? 0 : 1;
    }
    if (*s == 'a') {
        s++;
        if (!parse_uint(&s, &first)) first = buf->count;
        if (first > buf->count) return 1;
        mode->active = 1;
        mode->index = first;
        mode->replace_end = 0;
        return 0;
    }
    if (*s == 'i') {
        s++;
        if (!parse_uint(&s, &first)) first = buf->count ? buf->count : 1u;
        if (first == 0 || first > buf->count + 1u) return 1;
        mode->active = 1;
        mode->index = first - 1u;
        mode->replace_end = 0;
        return 0;
    }
    if (*s == 'r') {
        s++;
        if (!parse_uint(&s, &first) || first == 0 || first > buf->count) return 1;
        mode->active = 1;
        mode->index = first - 1u;
        mode->replace_end = first;
        return 0;
    }
    return 1;
}

static int batch(edit_buffer_t* buf, int argc, char** argv, int first_arg) {
    input_mode_t mode;
    int status = 0;

    mode.active = 0;
    mode.index = 0;
    mode.replace_end = 0;

    for (int i = first_arg; i < argc; i++) {
        if (!streq(argv[i], "-c") || i + 1 >= argc) {
            u_puts("usage: edit <path> [-c <command>]...\n");
            return 1;
        }
        i++;
        int r = batch_command(buf, &mode, argv[i]);
        if (r == 2) return status;
        if (r != 0) status = 1;
    }

    if (mode.active) {
        u_puts("edit: input ended before .\n");
        return 1;
    }
    return status;
}

void _start(int argc, char** argv) {
    edit_buffer_t buf;
    int status;

    if (argc < 2) {
        u_puts("usage: edit <path> [-c <command>]...\n");
        sys_exit(1);
    }

    editor_model_init(&buf, argv[1]);

    if (!load_file(&buf)) {
        free_buffer(&buf);
        sys_exit(1);
    }

    if (argc > 2) {
        status = batch(&buf, argc, argv, 2);
        if (status == 0) {
            u_puts("edit: wrote ");
            u_puts(buf.path);
            u_putc('\n');
        }
    } else {
        status = interactive(&buf);
    }

    free_buffer(&buf);
    sys_exit(status);
}
