#include "user_lib.h"

#define EDIT_LINE_MAX 256u
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

typedef struct {
    char** lines;
    uint32_t count;
    uint32_t cap;
    int dirty;
    const char* path;
} edit_buffer_t;

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

static char* alloc_line(const char* src) {
    char* out = (char*)malloc(EDIT_LINE_MAX);
    if (!out) return 0;

    uint32_t i = 0;
    while (src && src[i] && i < EDIT_LINE_MAX - 1u) {
        out[i] = src[i];
        i++;
    }
    out[i] = '\0';
    return out;
}

static void free_buffer(edit_buffer_t* buf) {
    if (!buf) return;
    for (uint32_t i = 0; i < buf->count; i++) {
        free(buf->lines[i]);
    }
    free(buf->lines);
    buf->lines = 0;
    buf->count = 0;
    buf->cap = 0;
}

static int reserve_lines(edit_buffer_t* buf, uint32_t needed) {
    if (needed <= buf->cap) return 1;

    uint32_t cap = buf->cap ? buf->cap : 16u;
    while (cap < needed) {
        cap *= 2u;
    }

    char** lines = (char**)realloc(buf->lines, cap * sizeof(char*));
    if (!lines) return 0;
    buf->lines = lines;
    buf->cap = cap;
    return 1;
}

static int insert_line(edit_buffer_t* buf, uint32_t index, const char* text) {
    if (index > buf->count) return 0;
    if (!reserve_lines(buf, buf->count + 1u)) return 0;

    char* copy = alloc_line(text);
    if (!copy) return 0;

    for (uint32_t i = buf->count; i > index; i--) {
        buf->lines[i] = buf->lines[i - 1u];
    }
    buf->lines[index] = copy;
    buf->count++;
    buf->dirty = 1;
    return 1;
}

static int delete_range(edit_buffer_t* buf, uint32_t first, uint32_t last) {
    uint32_t remove_count;

    if (first == 0 || last < first || first > buf->count) return 0;
    if (last > buf->count) last = buf->count;

    remove_count = last - first + 1u;
    for (uint32_t i = first - 1u; i < last; i++) {
        free(buf->lines[i]);
    }
    for (uint32_t i = last; i < buf->count; i++) {
        buf->lines[i - remove_count] = buf->lines[i];
    }
    buf->count -= remove_count;
    buf->dirty = 1;
    return 1;
}

static void clear_buffer(edit_buffer_t* buf) {
    for (uint32_t i = 0; i < buf->count; i++) {
        free(buf->lines[i]);
    }
    buf->count = 0;
    buf->dirty = 1;
}

static uint32_t line_len(edit_buffer_t* buf, uint32_t y) {
    if (y >= buf->count) return 0;
    return str_len(buf->lines[y]);
}

static int ensure_line(edit_buffer_t* buf, uint32_t y) {
    while (buf->count <= y) {
        if (!insert_line(buf, buf->count, "")) return 0;
    }
    return 1;
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

static int raw_read_char(char* out) {
    return sys_read_raw(out, 1u) == 1;
}

static int input_available(void) {
    struct pollfd pfd;
    pfd.fd = 0;
    pfd.events = POLLIN;
    pfd.revents = 0;
    return sys_poll(&pfd, 1u, 0) == 1 && (pfd.revents & POLLIN);
}

static key_t read_key(void) {
    key_t key;
    char c = 0;
    key.type = KEY_NONE;
    key.ch = 0;

    if (!raw_read_char(&c)) return key;

    if ((unsigned char)c != 27) {
        key.type = KEY_CHAR;
        key.ch = c;
        return key;
    }

    if (!input_available()) {
        key.type = KEY_ESC;
        return key;
    }

    char a = 0;
    if (!raw_read_char(&a)) {
        key.type = KEY_ESC;
        return key;
    }

    if (a == 'O') {
        char b = 0;
        if (!raw_read_char(&b)) return key;
        if (b == 'P') key.type = KEY_F1;
        else if (b == 'Q') key.type = KEY_F2;
        else if (b == 'R') key.type = KEY_F3;
        return key;
    }

    if (a != '[') {
        key.type = KEY_ESC;
        return key;
    }

    char b = 0;
    if (!raw_read_char(&b)) return key;
    if (b == 'A') key.type = KEY_UP;
    else if (b == 'B') key.type = KEY_DOWN;
    else if (b == 'C') key.type = KEY_RIGHT;
    else if (b == 'D') key.type = KEY_LEFT;
    else if (b == 'H') key.type = KEY_HOME;
    else if (b == 'F') key.type = KEY_END;
    else if (b >= '0' && b <= '9') {
        char tilde = 0;
        if (!raw_read_char(&tilde)) return key;
        if (tilde == '~') {
            if (b == '3') key.type = KEY_DELETE;
            else if (b == '5') key.type = KEY_PAGEUP;
            else if (b == '6') key.type = KEY_PAGEDOWN;
        } else if (b == '2' && tilde == '1') {
            char maybe_tilde = 0;
            if (raw_read_char(&maybe_tilde) && maybe_tilde == '~') {
                key.type = KEY_NONE;
            }
        }
    }
    return key;
}

static int save_file(edit_buffer_t* buf) {
    int fd = sys_open_mode(buf->path, SYS_OPEN_MODE_WRITE | SYS_OPEN_MODE_CREATE | SYS_OPEN_MODE_TRUNC);
    if (fd < 0) return 0;

    for (uint32_t i = 0; i < buf->count; i++) {
        uint32_t len = str_len(buf->lines[i]);
        if (len && sys_writefd(fd, buf->lines[i], len) != (int)len) {
            sys_close(fd);
            return 0;
        }
        if (sys_writefd(fd, "\n", 1u) != 1) {
            sys_close(fd);
            return 0;
        }
    }

    if (sys_fsync(fd) < 0) {
        sys_close(fd);
        return 0;
    }
    sys_close(fd);
    buf->dirty = 0;
    return 1;
}

static int load_file(edit_buffer_t* buf) {
    uint32_t size = 0;
    int is_dir = 0;
    int stat = u_stat(buf->path, &size, &is_dir);

    if (stat < 0) {
        return 1;
    }
    if (is_dir) {
        u_puts("edit: path is a directory\n");
        return 0;
    }

    int fd = sys_open(buf->path);
    if (fd < 0) {
        u_puts("edit: open failed\n");
        return 0;
    }

    char line[EDIT_LINE_MAX];
    uint32_t len = 0;
    char chunk[128];

    for (;;) {
        int n = sys_fread(fd, chunk, sizeof(chunk));
        if (n < 0) {
            sys_close(fd);
            u_puts("edit: read failed\n");
            return 0;
        }
        if (n == 0) break;

        for (int i = 0; i < n; i++) {
            char c = chunk[i];
            if (c == '\r') continue;
            if (c == '\n') {
                line[len] = '\0';
                if (!insert_line(buf, buf->count, line)) {
                    sys_close(fd);
                    u_puts("edit: out of memory\n");
                    return 0;
                }
                buf->dirty = 0;
                len = 0;
            } else if (len < EDIT_LINE_MAX - 1u) {
                line[len++] = c;
            }
        }
    }

    if (len > 0) {
        line[len] = '\0';
        if (!insert_line(buf, buf->count, line)) {
            sys_close(fd);
            u_puts("edit: out of memory\n");
            return 0;
        }
    }

    sys_close(fd);
    buf->dirty = 0;
    return 1;
}

static int line_insert_char(edit_buffer_t* buf, edit_view_t* view, char c) {
    if (!ensure_line(buf, view->cy)) return 0;

    char* line = buf->lines[view->cy];
    uint32_t len = str_len(line);
    if (len >= EDIT_LINE_MAX - 1u) return 0;
    if (view->cx > len) view->cx = len;

    for (uint32_t i = len + 1u; i > view->cx; i--) {
        line[i] = line[i - 1u];
    }
    line[view->cx] = c;
    view->cx++;
    buf->dirty = 1;
    return 1;
}

static int split_line(edit_buffer_t* buf, edit_view_t* view) {
    if (!ensure_line(buf, view->cy)) return 0;

    char* line = buf->lines[view->cy];
    uint32_t len = str_len(line);
    if (view->cx > len) view->cx = len;

    char* tail = alloc_line(&line[view->cx]);
    if (!tail) return 0;
    if (!reserve_lines(buf, buf->count + 1u)) {
        free(tail);
        return 0;
    }
    line[view->cx] = '\0';
    for (uint32_t i = buf->count; i > view->cy + 1u; i--) {
        buf->lines[i] = buf->lines[i - 1u];
    }
    buf->lines[view->cy + 1u] = tail;
    buf->count++;
    view->cy++;
    view->cx = 0;
    buf->dirty = 1;
    return 1;
}

static int join_with_next(edit_buffer_t* buf, uint32_t y) {
    if (y + 1u >= buf->count) return 0;
    uint32_t len = str_len(buf->lines[y]);
    uint32_t next_len = str_len(buf->lines[y + 1u]);
    if (len + next_len >= EDIT_LINE_MAX) return 0;

    for (uint32_t i = 0; i <= next_len; i++) {
        buf->lines[y][len + i] = buf->lines[y + 1u][i];
    }
    delete_range(buf, y + 2u, y + 2u);
    buf->dirty = 1;
    return 1;
}

static void backspace_key(edit_buffer_t* buf, edit_view_t* view) {
    if (buf->count == 0) return;
    if (view->cy >= buf->count) view->cy = buf->count - 1u;

    if (view->cx > 0) {
        char* line = buf->lines[view->cy];
        uint32_t len = str_len(line);
        if (view->cx > len) view->cx = len;
        for (uint32_t i = view->cx - 1u; i < len; i++) {
            line[i] = line[i + 1u];
        }
        view->cx--;
        buf->dirty = 1;
    } else if (view->cy > 0) {
        uint32_t prev_len = str_len(buf->lines[view->cy - 1u]);
        if (join_with_next(buf, view->cy - 1u)) {
            view->cy--;
            view->cx = prev_len;
        }
    }
}

static void delete_key(edit_buffer_t* buf, edit_view_t* view) {
    if (buf->count == 0 || view->cy >= buf->count) return;

    char* line = buf->lines[view->cy];
    uint32_t len = str_len(line);
    if (view->cx < len) {
        for (uint32_t i = view->cx; i < len; i++) {
            line[i] = line[i + 1u];
        }
        buf->dirty = 1;
    } else {
        join_with_next(buf, view->cy);
    }
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

    buf.lines = 0;
    buf.count = 0;
    buf.cap = 0;
    buf.dirty = 0;
    buf.path = argv[1];

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
