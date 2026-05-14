#include "shell_window.h"
#include "user_syscall.h"
#include "dirent.h"
#include "keyboard.h"

#define GUI_SHELL_NO_FD (-1)
#define GUI_SHELL_PIPE_FLAGS SYS_FD_FLAG_NONBLOCK
#define GUI_SHELL_SIGTERM 15
#define GUI_SHELL_PATH "/bin/shell.elf"
#define GUI_TREE_MAX_ENTRIES 64
#define GUI_TREE_MAX_DEPTH 6

typedef struct {
    char name[NAME_MAX + 1];
    int is_dir;
} gui_tree_entry_t;

static unsigned int s_len(const char* s) {
    unsigned int n = 0;
    while (s[n]) n++;
    return n;
}

static void s_copy(char* dst, const char* src, unsigned int cap) {
    unsigned int i = 0;
    while (i + 1 < cap && src[i]) {
        dst[i] = src[i];
        i++;
    }
    if (cap) dst[i] = 0;
}

static void s_cat(char* dst, const char* src, unsigned int cap) {
    unsigned int n = s_len(dst);
    while (n + 1 < cap && *src) {
        dst[n++] = *src++;
    }
    if (cap) dst[n] = 0;
}

static int s_eq(const char* a, const char* b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int s_starts_with(const char* s, const char* prefix) {
    while (*prefix) {
        if (*s++ != *prefix++) return 0;
    }
    return 1;
}

static int s_name_cmp(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) return (int)ca - (int)cb;
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static void utoa10_local(unsigned int v, char* buf) {
    char tmp[16];
    int n = 0;
    if (v == 0) {
        buf[0] = '0';
        buf[1] = 0;
        return;
    }
    while (v > 0 && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + v % 10u);
        v /= 10u;
    }
    int j = 0;
    while (n > 0) buf[j++] = tmp[--n];
    buf[j] = 0;
}

static void append_line(gui_shell_window_t* shell, const char* s) {
    if (shell->line_count >= GUI_SHELL_LINES) {
        for (int i = 0; i < GUI_SHELL_LINES - 1; i++) {
            for (int c = 0; c <= GUI_SHELL_COLS; c++) {
                shell->lines[i][c] = shell->lines[i + 1][c];
            }
        }
        shell->line_count = GUI_SHELL_LINES - 1;
    }
    char* dst = shell->lines[shell->line_count++];
    int i = 0;
    while (s[i] && i < GUI_SHELL_COLS) {
        dst[i] = s[i];
        i++;
    }
    dst[i] = 0;
    shell->scroll = 0;
}

static void terminal_blank_line(gui_shell_window_t* shell, int row) {
    if (row < 0 || row >= GUI_SHELL_LINES) return;
    for (int c = 0; c < GUI_SHELL_COLS; c++) shell->lines[row][c] = ' ';
    shell->lines[row][GUI_SHELL_COLS] = 0;
}

static void terminal_terminate_line(gui_shell_window_t* shell, int row) {
    if (row < 0 || row >= GUI_SHELL_LINES) return;
    shell->lines[row][GUI_SHELL_COLS] = 0;
}

static void terminal_ensure_row(gui_shell_window_t* shell, int row) {
    if (row < 0) return;
    if (row >= GUI_SHELL_LINES) row = GUI_SHELL_LINES - 1;
    while (shell->line_count <= row && shell->line_count < GUI_SHELL_LINES) {
        terminal_blank_line(shell, shell->line_count);
        shell->line_count++;
    }
}

static void terminal_scroll_one(gui_shell_window_t* shell) {
    for (int r = 0; r < GUI_SHELL_LINES - 1; r++) {
        for (int c = 0; c <= GUI_SHELL_COLS; c++) {
            shell->lines[r][c] = shell->lines[r + 1][c];
        }
    }
    terminal_blank_line(shell, GUI_SHELL_LINES - 1);
    shell->line_count = GUI_SHELL_LINES;
    if (shell->cursor_row > 0) shell->cursor_row--;
}

static void terminal_newline(gui_shell_window_t* shell) {
    terminal_terminate_line(shell, shell->cursor_row);
    shell->cursor_col = 0;
    shell->cursor_row++;
    if (shell->cursor_row >= GUI_SHELL_LINES) {
        terminal_scroll_one(shell);
    }
    terminal_ensure_row(shell, shell->cursor_row);
    if (shell->cursor_row == shell->line_count - 1) {
        terminal_blank_line(shell, shell->cursor_row);
    }
    shell->scroll = 0;
}

static void terminal_put_char(gui_shell_window_t* shell, char ch) {
    int cols = shell->term_cols;
    if (cols <= 0 || cols > GUI_SHELL_COLS) cols = GUI_SHELL_COLS;
    if (shell->cursor_col >= cols) terminal_newline(shell);
    if (shell->cursor_row < 0) shell->cursor_row = 0;
    if (shell->cursor_row >= GUI_SHELL_LINES) shell->cursor_row = GUI_SHELL_LINES - 1;
    terminal_ensure_row(shell, shell->cursor_row);
    if (shell->cursor_col < 0) shell->cursor_col = 0;
    if (shell->cursor_col >= GUI_SHELL_COLS) shell->cursor_col = GUI_SHELL_COLS - 1;
    shell->lines[shell->cursor_row][shell->cursor_col++] = ch;
    if (shell->cursor_col > cols) shell->cursor_col = cols;
    terminal_terminate_line(shell, shell->cursor_row);
}

static void terminal_clear_line(gui_shell_window_t* shell, int mode) {
    int cols = shell->term_cols;
    int start = 0;
    int end;
    if (cols <= 0 || cols > GUI_SHELL_COLS) cols = GUI_SHELL_COLS;
    end = cols;
    terminal_ensure_row(shell, shell->cursor_row);
    if (mode == 0) start = shell->cursor_col;
    else if (mode == 1) end = shell->cursor_col + 1;
    for (int c = start; c < end && c < GUI_SHELL_COLS; c++) {
        shell->lines[shell->cursor_row][c] = ' ';
    }
    terminal_terminate_line(shell, shell->cursor_row);
}

static void terminal_clear_screen(gui_shell_window_t* shell) {
    for (int r = 0; r < GUI_SHELL_LINES; r++) terminal_blank_line(shell, r);
    shell->line_count = shell->term_rows > 0 ? shell->term_rows : 1;
    if (shell->line_count > GUI_SHELL_LINES) shell->line_count = GUI_SHELL_LINES;
    shell->cursor_row = 0;
    shell->cursor_col = 0;
    shell->scroll = 0;
}

static void terminal_move_relative(gui_shell_window_t* shell, int dr, int dc) {
    int rows = shell->term_rows;
    int cols = shell->term_cols;
    if (rows <= 0 || rows > GUI_SHELL_LINES) rows = GUI_SHELL_LINES;
    if (cols <= 0 || cols > GUI_SHELL_COLS) cols = GUI_SHELL_COLS;
    shell->cursor_row += dr;
    shell->cursor_col += dc;
    if (shell->cursor_row < 0) shell->cursor_row = 0;
    if (shell->cursor_col < 0) shell->cursor_col = 0;
    if (shell->cursor_row >= GUI_SHELL_LINES) shell->cursor_row = GUI_SHELL_LINES - 1;
    if (shell->cursor_col >= cols) shell->cursor_col = cols - 1;
    terminal_ensure_row(shell, shell->cursor_row);
}

static void terminal_move_absolute(gui_shell_window_t* shell, int row, int col) {
    int base = shell->line_count - shell->term_rows;
    if (base < 0) base = 0;
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    shell->cursor_row = base + row;
    shell->cursor_col = col;
    if (shell->cursor_row >= GUI_SHELL_LINES) shell->cursor_row = GUI_SHELL_LINES - 1;
    if (shell->cursor_col >= shell->term_cols) shell->cursor_col = shell->term_cols - 1;
    if (shell->cursor_col < 0) shell->cursor_col = 0;
    terminal_ensure_row(shell, shell->cursor_row);
}

static void terminal_reset(gui_shell_window_t* shell) {
    if (shell->term_rows <= 0) shell->term_rows = 25;
    if (shell->term_cols <= 0) shell->term_cols = GUI_SHELL_COLS;
    terminal_clear_screen(shell);
}

void gui_shell_set_terminal_size(gui_shell_window_t* shell,
                                 unsigned int rows,
                                 unsigned int cols) {
    if (!shell) return;
    if (rows == 0) rows = 1;
    if (cols == 0) cols = 1;
    if (rows > GUI_SHELL_LINES) rows = GUI_SHELL_LINES;
    if (cols > GUI_SHELL_COLS) cols = GUI_SHELL_COLS;
    if (shell->term_rows == (int)rows && shell->term_cols == (int)cols) return;
    shell->term_rows = (int)rows;
    shell->term_cols = (int)cols;
    if (shell->backend == GUI_SHELL_BACKEND_PTY_CHILD && shell->stdin_fd >= 0) {
        (void)sys_pty_set_size(shell->stdin_fd, rows, cols);
    }
    if (shell->line_count == 0) terminal_reset(shell);
}

static void pending_clear(gui_shell_window_t* shell) {
    shell->pending[0] = 0;
    shell->pending_len = 0;
    shell->pending_cursor = 0;
}

static void pending_push(gui_shell_window_t* shell, char ch) {
    if (shell->pending_cursor >= GUI_SHELL_COLS) {
        append_line(shell, shell->pending);
        pending_clear(shell);
    }
    shell->pending[shell->pending_cursor++] = ch;
    if (shell->pending_cursor > shell->pending_len) {
        shell->pending_len = shell->pending_cursor;
    }
    shell->pending[shell->pending_len] = 0;
}

static void pending_flush(gui_shell_window_t* shell) {
    if (shell->pending_len <= 0) return;
    append_line(shell, shell->pending);
    pending_clear(shell);
}

static void terminal_csi_dispatch(gui_shell_window_t* shell, char cmd) {
    int arg0 = shell->csi_count > 0 ? shell->csi_args[0] : (shell->csi_has_value ? shell->csi_value : 0);
    int arg1 = shell->csi_count > 1 ? shell->csi_args[1] : 0;
    int n = arg0 ? arg0 : 1;
    if (n < 1) n = 1;

    if (cmd == 'C') {
        if (shell->backend == GUI_SHELL_BACKEND_PTY_CHILD) terminal_move_relative(shell, 0, n);
        else shell->pending_cursor += n;
    } else if (cmd == 'D') {
        if (shell->backend == GUI_SHELL_BACKEND_PTY_CHILD) terminal_move_relative(shell, 0, -n);
        else {
            shell->pending_cursor -= n;
            if (shell->pending_cursor < 0) shell->pending_cursor = 0;
        }
    } else if (cmd == 'A') {
        if (shell->backend == GUI_SHELL_BACKEND_PTY_CHILD) terminal_move_relative(shell, -n, 0);
    } else if (cmd == 'B') {
        if (shell->backend == GUI_SHELL_BACKEND_PTY_CHILD) terminal_move_relative(shell, n, 0);
    } else if (cmd == 'K') {
        if (shell->backend == GUI_SHELL_BACKEND_PTY_CHILD) {
            terminal_clear_line(shell, arg0);
        } else {
            if (shell->pending_cursor < 0) shell->pending_cursor = 0;
            if (shell->pending_cursor < shell->pending_len) {
                shell->pending_len = shell->pending_cursor;
                shell->pending[shell->pending_len] = 0;
            }
        }
    } else if (cmd == 'J') {
        if (shell->backend == GUI_SHELL_BACKEND_PTY_CHILD) {
            if (arg0 == 0 || arg0 == 2) terminal_clear_screen(shell);
        } else {
            if (!shell->csi_has_value || shell->csi_value == 2) {
                shell->line_count = 0;
                shell->scroll = 0;
                pending_clear(shell);
            }
        }
    } else if (cmd == 'H' || cmd == 'f') {
        if (shell->backend == GUI_SHELL_BACKEND_PTY_CHILD) {
            terminal_move_absolute(shell, (arg0 ? arg0 : 1) - 1, (arg1 ? arg1 : 1) - 1);
        } else {
            shell->pending_cursor = 0;
        }
    } else if (cmd == 'F') {
        if (cmd == 'H') shell->pending_cursor = 0;
        else shell->pending_cursor = shell->pending_len;
    }
}

static int terminal_handle_escape(gui_shell_window_t* shell, char ch) {
    if (shell->esc_state == 0) {
        if ((unsigned char)ch == 27u) {
            shell->esc_state = 1;
            return 1;
        }
        return 0;
    }

    if (shell->esc_state == 1) {
        if (ch == '[') {
            shell->esc_state = 2;
            shell->csi_count = 0;
            shell->csi_value = 0;
            shell->csi_has_value = 0;
            shell->csi_private = 0;
            for (int i = 0; i < GUI_SHELL_CSI_ARGS; i++) shell->csi_args[i] = 0;
            return 1;
        }
        shell->esc_state = 0;
        return 1;
    }

    if (shell->esc_state == 2) {
        if (ch == '?') {
            shell->csi_private = 1;
            return 1;
        }
        if (ch >= '0' && ch <= '9') {
            shell->csi_value = shell->csi_value * 10 + (ch - '0');
            shell->csi_has_value = 1;
            return 1;
        }
        if (ch == ';') {
            if (shell->csi_count < GUI_SHELL_CSI_ARGS) {
                shell->csi_args[shell->csi_count++] = shell->csi_has_value ? shell->csi_value : 0;
            }
            shell->csi_value = 0;
            shell->csi_has_value = 0;
            return 1;
        }
        if (shell->csi_count < GUI_SHELL_CSI_ARGS) {
            shell->csi_args[shell->csi_count++] = shell->csi_has_value ? shell->csi_value : 0;
        }
        terminal_csi_dispatch(shell, ch);
        shell->esc_state = 0;
        return 1;
    }

    shell->esc_state = 0;
    return 0;
}

static char gui_shell_map_codepoint(unsigned int cp) {
    switch (cp) {
        case 0x2500u: return '-'; /* box drawings light horizontal */
        case 0x2502u: return '|'; /* box drawings light vertical */
        case 0x2514u: return '`'; /* box drawings light up and right */
        case 0x251Cu: return '+'; /* box drawings light vertical and right */
        default: break;
    }
    if (cp >= 32u && cp < 127u) return (char)cp;
    return '?';
}

static char gui_shell_map_legacy_byte(unsigned char ch) {
    switch (ch) {
        case 179u: return '|'; /* CP437 vertical */
        case 192u: return '`'; /* CP437 lower-left */
        case 195u: return '+'; /* CP437 tee-right */
        case 196u: return '-'; /* CP437 horizontal */
        default: break;
    }
    return '?';
}

static int gui_shell_utf8_expected(unsigned char ch) {
    if ((ch & 0xE0u) == 0xC0u) return 2;
    if ((ch & 0xF0u) == 0xE0u) return 3;
    if ((ch & 0xF8u) == 0xF0u) return 4;
    return 0;
}

static unsigned int gui_shell_utf8_decode(const unsigned char* buf, int len) {
    if (len == 2) {
        return ((unsigned int)(buf[0] & 0x1Fu) << 6) |
               (unsigned int)(buf[1] & 0x3Fu);
    }
    if (len == 3) {
        return ((unsigned int)(buf[0] & 0x0Fu) << 12) |
               ((unsigned int)(buf[1] & 0x3Fu) << 6) |
               (unsigned int)(buf[2] & 0x3Fu);
    }
    if (len == 4) {
        return ((unsigned int)(buf[0] & 0x07u) << 18) |
               ((unsigned int)(buf[1] & 0x3Fu) << 12) |
               ((unsigned int)(buf[2] & 0x3Fu) << 6) |
               (unsigned int)(buf[3] & 0x3Fu);
    }
    return 0xFFFDu;
}

static int gui_shell_translate_printable(gui_shell_window_t* shell,
                                         unsigned char byte,
                                         char* out) {
    if (!out) return 0;

    if (shell->utf8_len > 0) {
        if ((byte & 0xC0u) != 0x80u) {
            shell->utf8_len = 0;
            shell->utf8_need = 0;
            *out = gui_shell_map_legacy_byte(byte);
            return 1;
        }
        shell->utf8_buf[shell->utf8_len++] = byte;
        if (shell->utf8_len < shell->utf8_need) return 0;

        *out = gui_shell_map_codepoint(gui_shell_utf8_decode(shell->utf8_buf,
                                                             shell->utf8_len));
        shell->utf8_len = 0;
        shell->utf8_need = 0;
        return 1;
    }

    if (byte < 0x80u) {
        if (byte < 32u) return 0;
        *out = (char)byte;
        return 1;
    }

    shell->utf8_need = gui_shell_utf8_expected(byte);
    if (shell->utf8_need > 0) {
        shell->utf8_buf[0] = byte;
        shell->utf8_len = 1;
        return 0;
    }

    *out = gui_shell_map_legacy_byte(byte);
    return 1;
}

static void print_stream_text(gui_shell_window_t* shell, const char* s, int flush_tail) {
    while (*s) {
        if (terminal_handle_escape(shell, *s)) {
            s++;
            continue;
        }
        if (shell->backend == GUI_SHELL_BACKEND_PTY_CHILD) {
            if (*s == '\n') {
                terminal_newline(shell);
            } else if (*s == '\r') {
                shell->cursor_col = 0;
            } else if (*s == '\t') {
                do {
                    terminal_put_char(shell, ' ');
                } while ((shell->cursor_col % 4) != 0);
            } else if (*s == '\b' || (unsigned char)*s == 127u) {
                if (shell->cursor_col > 0) shell->cursor_col--;
            } else {
                char out = 0;
                if (gui_shell_translate_printable(shell, (unsigned char)*s, &out)) {
                    terminal_put_char(shell, out);
                }
            }
        } else if (*s == '\n') {
            append_line(shell, shell->pending);
            pending_clear(shell);
        } else if (*s == '\r') {
            shell->pending_cursor = 0;
        } else if (*s == '\t') {
            do {
                pending_push(shell, ' ');
            } while ((shell->pending_cursor % 4) != 0);
        } else if (*s == '\b' || (unsigned char)*s == 127u) {
            if (shell->pending_cursor > 0) {
                shell->pending_cursor--;
                shell->pending_len = shell->pending_cursor;
                shell->pending[shell->pending_len] = 0;
            }
        } else {
            char out = 0;
            if (gui_shell_translate_printable(shell, (unsigned char)*s, &out)) {
                pending_push(shell, out);
            }
        }
        s++;
    }
    if (flush_tail && shell->backend != GUI_SHELL_BACKEND_PTY_CHILD) pending_flush(shell);
}

static void print_text(gui_shell_window_t* shell, const char* s) {
    print_stream_text(shell, s, 1);
}

static void print_child_text(gui_shell_window_t* shell, const char* s) {
    print_stream_text(shell, s, 0);
}

void gui_shell_format_prompt(gui_shell_window_t* shell,
                             char* out,
                             unsigned int cap) {
    if (shell->backend == GUI_SHELL_BACKEND_PIPE_CHILD) {
        if (shell->pending_len > 0) {
            s_copy(out, shell->pending, cap);
        } else {
            s_copy(out, "$ ", cap);
        }
        s_cat(out, shell->input, cap);
        return;
    }
    if (shell->backend == GUI_SHELL_BACKEND_PTY_CHILD) {
        if (shell->pending_len > 0) {
            s_copy(out, shell->pending, cap);
        } else {
            s_copy(out, "", cap);
        }
        return;
    }
    s_copy(out, shell->cwd, cap);
    s_cat(out, " $ ", cap);
    s_cat(out, shell->input, cap);
}

static int tokenize(char* line, char* argv[], int max_argv) {
    int argc = 0;
    char* p = line;
    while (*p && argc < max_argv) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) {
            *p = 0;
            p++;
        }
    }
    return argc;
}

static void resolve_path(const char* cwd, const char* rel, char* out, unsigned int cap) {
    if (rel[0] == '/') {
        s_copy(out, rel, cap);
    } else {
        s_copy(out, cwd, cap);
        unsigned int n = s_len(out);
        if (n == 0 || out[n - 1] != '/') s_cat(out, "/", cap);
        s_cat(out, rel, cap);
    }

    char tmp[256];
    s_copy(tmp, out, sizeof(tmp));
    out[0] = '/';
    out[1] = 0;
    unsigned int outn = 1;
    unsigned int i = 0;
    while (tmp[i] == '/') i++;
    while (tmp[i]) {
        char comp[64];
        unsigned int cn = 0;
        while (tmp[i] && tmp[i] != '/' && cn + 1 < sizeof(comp)) {
            comp[cn++] = tmp[i++];
        }
        comp[cn] = 0;
        while (tmp[i] == '/') i++;
        if (cn == 0) continue;
        if (comp[0] == '.' && comp[1] == 0) continue;
        if (comp[0] == '.' && comp[1] == '.' && comp[2] == 0) {
            if (outn > 1) {
                outn--;
                while (outn > 0 && out[outn - 1] != '/') outn--;
                out[outn] = 0;
            }
            continue;
        }
        if (outn > 1 && out[outn - 1] != '/') {
            if (outn + 1 < cap) {
                out[outn++] = '/';
                out[outn] = 0;
            }
        }
        unsigned int j = 0;
        while (comp[j] && outn + 1 < cap) out[outn++] = comp[j++];
        out[outn] = 0;
        if (outn + 1 < cap) {
            out[outn++] = '/';
            out[outn] = 0;
        }
    }

    unsigned int fn = s_len(out);
    if (fn > 1 && out[fn - 1] == '/') out[fn - 1] = 0;
}

static void cmd_help(gui_shell_window_t* shell) {
    append_line(shell, "recovery commands:");
    append_line(shell, "  pwd            print current directory");
    append_line(shell, "  cd [path]      change directory (default /)");
    append_line(shell, "  ls [path]      list directory");
    append_line(shell, "  tree [path]    print directory tree");
    append_line(shell, "  cat <file>     print file contents");
    append_line(shell, "  echo <args>    print arguments");
    append_line(shell, "  clear          clear scrollback");
    append_line(shell, "  help           show this list");
    append_line(shell, "  exit           close shell");
}

static void cmd_cd(gui_shell_window_t* shell, int argc, char* argv[]) {
    char target[256];
    if (argc < 2) {
        s_copy(target, "/", sizeof(target));
    } else {
        resolve_path(shell->cwd, argv[1], target, sizeof(target));
    }
    uint32_t size = 0;
    int is_dir = 0;
    if (sys_stat(target, &size, &is_dir) < 0) {
        append_line(shell, "cd: no such path");
        return;
    }
    if (!is_dir) {
        append_line(shell, "cd: not a directory");
        return;
    }
    s_copy(shell->cwd, target, sizeof(shell->cwd));
}

static void cmd_ls(gui_shell_window_t* shell, int argc, char* argv[]) {
    char target[256];
    if (argc < 2) {
        s_copy(target, shell->cwd, sizeof(target));
    } else {
        resolve_path(shell->cwd, argv[1], target, sizeof(target));
    }
    DIR* d = opendir(target);
    if (!d) {
        append_line(shell, "ls: cannot open");
        return;
    }
    struct dirent* e;
    while ((e = readdir(d)) != 0) {
        char line[GUI_SHELL_COLS + 1];
        s_copy(line, e->d_is_dir ? "d " : "  ", sizeof(line));
        s_cat(line, e->d_name, sizeof(line));
        if (!e->d_is_dir) {
            char num[16];
            s_cat(line, "  (", sizeof(line));
            utoa10_local(e->d_size, num);
            s_cat(line, num, sizeof(line));
            s_cat(line, " bytes)", sizeof(line));
        }
        append_line(shell, line);
    }
    closedir(d);
}

static int tree_entry_cmp(const gui_tree_entry_t* a, const gui_tree_entry_t* b) {
    if (a->is_dir != b->is_dir) return a->is_dir ? -1 : 1;
    return s_name_cmp(a->name, b->name);
}

static void tree_sort_entries(gui_tree_entry_t* entries, unsigned int count) {
    for (unsigned int i = 1; i < count; i++) {
        gui_tree_entry_t key = entries[i];
        unsigned int j = i;
        while (j > 0 && tree_entry_cmp(&key, &entries[j - 1]) < 0) {
            entries[j] = entries[j - 1];
            j--;
        }
        entries[j] = key;
    }
}

static void tree_join_path(const char* parent,
                           const char* name,
                           char* out,
                           unsigned int cap) {
    s_copy(out, parent, cap);
    if (!out[0]) s_copy(out, "/", cap);
    if (!(out[0] == '/' && out[1] == 0)) s_cat(out, "/", cap);
    s_cat(out, name, cap);
}

static void tree_walk(gui_shell_window_t* shell,
                      const char* path,
                      const char* prefix,
                      unsigned int depth) {
    gui_tree_entry_t entries[GUI_TREE_MAX_ENTRIES];
    unsigned int count = 0;
    int truncated = 0;
    DIR* d;

    if (depth >= GUI_TREE_MAX_DEPTH) {
        char line[GUI_SHELL_COLS + 1];
        s_copy(line, prefix, sizeof(line));
        s_cat(line, "`-- ...", sizeof(line));
        append_line(shell, line);
        return;
    }

    d = opendir(path);
    if (!d) return;

    struct dirent* e;
    while ((e = readdir(d)) != 0) {
        if (count >= GUI_TREE_MAX_ENTRIES) {
            truncated = 1;
            break;
        }
        if (s_eq(e->d_name, ".") || s_eq(e->d_name, "..")) continue;
        s_copy(entries[count].name, e->d_name, sizeof(entries[count].name));
        entries[count].is_dir = e->d_is_dir;
        count++;
    }
    closedir(d);

    tree_sort_entries(entries, count);

    for (unsigned int i = 0; i < count; i++) {
        int last = (i + 1 == count) && !truncated;
        char line[GUI_SHELL_COLS + 1];
        s_copy(line, prefix, sizeof(line));
        s_cat(line, last ? "`-- " : "|-- ", sizeof(line));
        s_cat(line, entries[i].name, sizeof(line));
        if (entries[i].is_dir) s_cat(line, "/", sizeof(line));
        append_line(shell, line);

        if (entries[i].is_dir) {
            char child_path[256];
            char child_prefix[GUI_SHELL_COLS + 1];
            tree_join_path(path, entries[i].name, child_path, sizeof(child_path));
            s_copy(child_prefix, prefix, sizeof(child_prefix));
            s_cat(child_prefix, last ? "    " : "|   ", sizeof(child_prefix));
            tree_walk(shell, child_path, child_prefix, depth + 1);
        }
    }

    if (truncated) {
        char line[GUI_SHELL_COLS + 1];
        s_copy(line, prefix, sizeof(line));
        s_cat(line, "`-- ...", sizeof(line));
        append_line(shell, line);
    }
}

static void cmd_tree(gui_shell_window_t* shell, int argc, char* argv[]) {
    char target[256];
    uint32_t size = 0;
    int is_dir = 0;

    if (argc < 2) {
        s_copy(target, shell->cwd, sizeof(target));
    } else {
        resolve_path(shell->cwd, argv[1], target, sizeof(target));
    }

    if (sys_stat(target, &size, &is_dir) < 0) {
        append_line(shell, "tree: cannot open");
        return;
    }
    if (!is_dir) {
        append_line(shell, "tree: not a directory");
        return;
    }

    append_line(shell, target);
    tree_walk(shell, target, "", 0);
}

static void cmd_cat(gui_shell_window_t* shell, int argc, char* argv[]) {
    if (argc < 2) {
        append_line(shell, "cat: need a path");
        return;
    }
    char target[256];
    resolve_path(shell->cwd, argv[1], target, sizeof(target));
    int fd = sys_open(target);
    if (fd < 0) {
        append_line(shell, "cat: cannot open");
        return;
    }
    char buf[256];
    for (;;) {
        int n = sys_fread(fd, buf, sizeof(buf) - 1);
        if (n <= 0) break;
        buf[n] = 0;
        print_text(shell, buf);
    }
    sys_close(fd);
    if (shell->line_count > 0) {
        int idx = shell->line_count - 1;
        if (shell->lines[idx][0] != 0) append_line(shell, "");
    }
}

static void cmd_echo(gui_shell_window_t* shell, int argc, char* argv[]) {
    char line[GUI_SHELL_COLS + 1];
    line[0] = 0;
    for (int i = 1; i < argc; i++) {
        if (i > 1) s_cat(line, " ", sizeof(line));
        s_cat(line, argv[i], sizeof(line));
    }
    append_line(shell, line);
}

static const char* const gui_shell_builtins[] = {
    "help", "clear", "meminfo", "memmap", "netinfo", "dhcp",
    "netsend", "netrecv", "arpgw", "ping", "pinggw", "pingpublic",
    "netcheck", "mousetest", "ataread", "cd", "pwd", "wd", "runelf",
    "runelf_nowait", "runelf_bg", "bg", "jobs", "fg", "kill",
    "selftest", "shelltest", "echo", "about", "halt", "reboot",
    "uptime", "ls", "tree", "fsread", "cat", "more", "mkdir",
    "rmdir", "rm", "touch", "cp", "mv", "edit", "exit",
};

static void gui_shell_history_add(gui_shell_window_t* shell, const char* line) {
    if (!line || !line[0]) return;
    if (shell->history_count > 0 &&
        s_eq(shell->history[(shell->history_count - 1) % GUI_SHELL_HISTORY], line)) {
        return;
    }
    s_copy(shell->history[shell->history_count % GUI_SHELL_HISTORY],
           line,
           GUI_SHELL_INPUT);
    shell->history_count++;
}

static unsigned int gui_shell_history_available(gui_shell_window_t* shell) {
    return shell->history_count < GUI_SHELL_HISTORY
        ? shell->history_count
        : GUI_SHELL_HISTORY;
}

static const char* gui_shell_history_at(gui_shell_window_t* shell, unsigned int index) {
    unsigned int avail = gui_shell_history_available(shell);
    unsigned int first;
    if (avail == 0 || index >= avail) return 0;
    first = shell->history_count - avail;
    return shell->history[(first + index) % GUI_SHELL_HISTORY];
}

static void gui_shell_set_input(gui_shell_window_t* shell, const char* text) {
    s_copy(shell->input, text ? text : "", sizeof(shell->input));
    shell->input_len = (int)s_len(shell->input);
}

static void gui_shell_common_prefix(char* common, const char* next) {
    unsigned int i = 0;
    while (common[i] && next[i] && common[i] == next[i]) i++;
    common[i] = 0;
}

static void gui_shell_match_add(const char* candidate,
                                const char* prefix,
                                unsigned int* count,
                                char* common,
                                unsigned int common_cap) {
    if (!candidate || !s_starts_with(candidate, prefix)) return;
    if (*count == 0) {
        s_copy(common, candidate, common_cap);
    } else {
        gui_shell_common_prefix(common, candidate);
    }
    (*count)++;
}

static void gui_shell_match_program_dir(const char* path,
                                        const char* prefix,
                                        unsigned int* count,
                                        char* common,
                                        unsigned int common_cap) {
    DIR* dir = opendir(path);
    if (!dir) return;
    struct dirent* ent;
    while ((ent = readdir(dir)) != 0) {
        char name[GUI_SHELL_INPUT];
        unsigned int len;
        if (ent->d_is_dir || !ent->d_name[0]) continue;
        s_copy(name, ent->d_name, sizeof(name));
        len = s_len(name);
        if (len > 4 &&
            name[len - 4] == '.' &&
            name[len - 3] == 'e' &&
            name[len - 2] == 'l' &&
            name[len - 1] == 'f') {
            name[len - 4] = 0;
        }
        gui_shell_match_add(name, prefix, count, common, common_cap);
    }
    closedir(dir);
}

static int gui_shell_command_token(const char* input, unsigned int start) {
    for (unsigned int i = 0; i < start; i++) {
        if (input[i] != ' ' && input[i] != '\t') return 0;
    }
    return 1;
}

static void gui_shell_token_parts(const char* token,
                                  char* dir_token,
                                  unsigned int dir_cap,
                                  char* leaf,
                                  unsigned int leaf_cap,
                                  char* output_prefix,
                                  unsigned int output_cap) {
    int slash = -1;
    unsigned int len = s_len(token);
    for (unsigned int i = 0; i < len; i++) {
        if (token[i] == '/') slash = (int)i;
    }
    if (slash < 0) {
        s_copy(dir_token, ".", dir_cap);
        s_copy(leaf, token, leaf_cap);
        s_copy(output_prefix, "", output_cap);
        return;
    }
    if (slash == 0) {
        s_copy(dir_token, "/", dir_cap);
    } else {
        unsigned int n = 0;
        while (n < (unsigned int)slash && n + 1 < dir_cap) {
            dir_token[n] = token[n];
            n++;
        }
        dir_token[n] = 0;
    }
    {
        unsigned int n = 0;
        while (n <= (unsigned int)slash && n + 1 < output_cap) {
            output_prefix[n] = token[n];
            n++;
        }
        output_prefix[n] = 0;
    }
    s_copy(leaf, token + slash + 1, leaf_cap);
}

static void gui_shell_match_path(gui_shell_window_t* shell,
                                 const char* token,
                                 unsigned int* count,
                                 char* common,
                                 unsigned int common_cap) {
    char dir_token[GUI_SHELL_INPUT];
    char dir_path[GUI_SHELL_INPUT];
    char leaf[GUI_SHELL_INPUT];
    char output_prefix[GUI_SHELL_INPUT];
    DIR* dir;

    gui_shell_token_parts(token, dir_token, sizeof(dir_token),
                          leaf, sizeof(leaf),
                          output_prefix, sizeof(output_prefix));
    resolve_path(shell->cwd, dir_token, dir_path, sizeof(dir_path));
    dir = opendir(dir_path);
    if (!dir) return;

    struct dirent* ent;
    while ((ent = readdir(dir)) != 0) {
        char candidate[GUI_SHELL_INPUT];
        if (!ent->d_name[0] || !s_starts_with(ent->d_name, leaf)) continue;
        s_copy(candidate, output_prefix, sizeof(candidate));
        s_cat(candidate, ent->d_name, sizeof(candidate));
        if (ent->d_is_dir) s_cat(candidate, "/", sizeof(candidate));
        gui_shell_match_add(candidate, "", count, common, common_cap);
    }
    closedir(dir);
}

static void gui_shell_complete(gui_shell_window_t* shell) {
    unsigned int start = (unsigned int)shell->input_len;
    char token[GUI_SHELL_INPUT];
    char common[GUI_SHELL_INPUT];
    unsigned int count = 0;
    unsigned int token_len;

    while (start > 0 &&
           shell->input[start - 1] != ' ' &&
           shell->input[start - 1] != '\t') {
        start--;
    }

    token[0] = 0;
    for (unsigned int i = start; i < (unsigned int)shell->input_len && i - start + 1 < sizeof(token); i++) {
        token[i - start] = shell->input[i];
        token[i - start + 1] = 0;
    }
    common[0] = 0;
    token_len = s_len(token);

    if (gui_shell_command_token(shell->input, start)) {
        for (unsigned int i = 0; i < sizeof(gui_shell_builtins) / sizeof(gui_shell_builtins[0]); i++) {
            gui_shell_match_add(gui_shell_builtins[i], token, &count, common, sizeof(common));
        }
        gui_shell_match_program_dir("/bin", token, &count, common, sizeof(common));
        gui_shell_match_program_dir("/usr/bin", token, &count, common, sizeof(common));
        gui_shell_match_program_dir("/usr/sbin", token, &count, common, sizeof(common));
    } else {
        gui_shell_match_path(shell, token, &count, common, sizeof(common));
    }

    if (count == 0) return;
    if (s_len(common) <= token_len && count != 1) return;
    if (start + s_len(common) + 2 >= sizeof(shell->input)) return;

    shell->input[start] = 0;
    s_cat(shell->input, common, sizeof(shell->input));
    shell->input_len = (int)s_len(shell->input);
    if (count == 1 &&
        shell->input_len + 1 < GUI_SHELL_INPUT &&
        (shell->input_len == 0 || shell->input[shell->input_len - 1] != '/')) {
        shell->input[shell->input_len++] = ' ';
        shell->input[shell->input_len] = 0;
    }
}

static void gui_shell_track_cd(gui_shell_window_t* shell, const char* input) {
    char line[GUI_SHELL_INPUT];
    char* argv[3];
    int argc;
    char target[256];
    uint32_t size = 0;
    int is_dir = 0;

    s_copy(line, input, sizeof(line));
    argc = tokenize(line, argv, 3);
    if (argc == 0 || !s_eq(argv[0], "cd")) return;
    if (argc < 2) {
        s_copy(target, "/", sizeof(target));
    } else {
        resolve_path(shell->cwd, argv[1], target, sizeof(target));
    }
    if (sys_stat(target, &size, &is_dir) == 0 && is_dir) {
        s_copy(shell->cwd, target, sizeof(shell->cwd));
    }
}

static gui_shell_key_result_t run_command(gui_shell_window_t* shell) {
    char prompt[GUI_SHELL_COLS + 1];
    gui_shell_format_prompt(shell, prompt, sizeof(prompt));
    append_line(shell, prompt);

    char line[GUI_SHELL_INPUT];
    s_copy(line, shell->input, sizeof(line));

    char* argv[16];
    int argc = tokenize(line, argv, 16);
    if (argc == 0) goto done;

    if (s_eq(argv[0], "help"))       cmd_help(shell);
    else if (s_eq(argv[0], "pwd"))   append_line(shell, shell->cwd);
    else if (s_eq(argv[0], "cd"))    cmd_cd(shell, argc, argv);
    else if (s_eq(argv[0], "ls"))    cmd_ls(shell, argc, argv);
    else if (s_eq(argv[0], "tree"))  cmd_tree(shell, argc, argv);
    else if (s_eq(argv[0], "cat"))   cmd_cat(shell, argc, argv);
    else if (s_eq(argv[0], "echo"))  cmd_echo(shell, argc, argv);
    else if (s_eq(argv[0], "clear")) { shell->line_count = 0; shell->scroll = 0; }
    else if (s_eq(argv[0], "exit"))  return GUI_SHELL_KEY_CLOSE;
    else {
        char msg[GUI_SHELL_COLS + 1];
        s_copy(msg, "unknown command: ", sizeof(msg));
        s_cat(msg, argv[0], sizeof(msg));
        s_cat(msg, " (try 'help')", sizeof(msg));
        append_line(shell, msg);
    }

done:
    shell->input[0] = 0;
    shell->input_len = 0;
    return GUI_SHELL_KEY_OK;
}

void gui_shell_open(gui_shell_window_t* shell) {
    for (unsigned int i = 0; i < sizeof(*shell); i++) {
        ((char*)shell)[i] = 0;
    }
    s_copy(shell->cwd, "/", sizeof(shell->cwd));
    shell->term_rows = 25;
    shell->term_cols = GUI_SHELL_COLS;
    shell->backend = GUI_SHELL_BACKEND_PIPE_CHILD;
    shell->pid = -1;
    shell->stdin_fd = GUI_SHELL_NO_FD;
    shell->stdout_fd = GUI_SHELL_NO_FD;

    int pty_fds[2];
    if (sys_pty_open(pty_fds, SYS_FD_FLAG_NONBLOCK) == 0) {
        int pid = sys_fork();
        if (pid < 0) {
            sys_close(pty_fds[0]);
            sys_close(pty_fds[1]);
        } else if (pid == 0) {
            char* argv[] = { "shell", 0 };
            sys_dup2(pty_fds[1], 0);
            sys_dup2(pty_fds[1], 1);
            sys_dup2(pty_fds[1], 2);
            sys_close(pty_fds[0]);
            sys_close(pty_fds[1]);
            sys_execve(GUI_SHELL_PATH, argv, 0);
            sys_exit(127);
        } else {
            sys_close(pty_fds[1]);
            shell->backend = GUI_SHELL_BACKEND_PTY_CHILD;
            shell->pid = pid;
            shell->stdin_fd = pty_fds[0];
            shell->stdout_fd = pty_fds[0];
            append_line(shell, "SmallOS shell window");
            append_line(shell, "backend: PTY " GUI_SHELL_PATH " child process");
            shell->cursor_row = shell->line_count;
            shell->cursor_col = 0;
            terminal_ensure_row(shell, shell->cursor_row);
            (void)sys_pty_set_size(shell->stdin_fd,
                                   (unsigned int)shell->term_rows,
                                   (unsigned int)shell->term_cols);
            gui_shell_poll(shell);
            return;
        }
    }

    int in_pipe[2];
    int out_pipe[2];
    if (sys_pipe(in_pipe) < 0) goto embedded_fallback;
    if (sys_pipe2(out_pipe, GUI_SHELL_PIPE_FLAGS) < 0) {
        sys_close(in_pipe[0]);
        sys_close(in_pipe[1]);
        goto embedded_fallback;
    }

    int pid = sys_fork();
    if (pid < 0) {
        sys_close(in_pipe[0]);
        sys_close(in_pipe[1]);
        sys_close(out_pipe[0]);
        sys_close(out_pipe[1]);
        goto embedded_fallback;
    }

    if (pid == 0) {
        char* argv[] = { "shell", "--line-mode", 0 };
        sys_dup2(in_pipe[0], 0);
        sys_dup2(out_pipe[1], 1);
        sys_dup2(out_pipe[1], 2);
        sys_close(in_pipe[0]);
        sys_close(in_pipe[1]);
        sys_close(out_pipe[0]);
        sys_close(out_pipe[1]);
        sys_execve(GUI_SHELL_PATH, argv, 0);
        sys_exit(127);
    }

    sys_close(in_pipe[0]);
    sys_close(out_pipe[1]);
    shell->pid = pid;
    shell->stdin_fd = in_pipe[1];
    shell->stdout_fd = out_pipe[0];
    append_line(shell, "SmallOS shell window");
    append_line(shell, "backend: " GUI_SHELL_PATH " child process");
    gui_shell_poll(shell);
    return;

embedded_fallback:
    shell->backend = GUI_SHELL_BACKEND_EMBEDDED;
    shell->pid = -1;
    shell->stdin_fd = GUI_SHELL_NO_FD;
    shell->stdout_fd = GUI_SHELL_NO_FD;
    append_line(shell, "SmallOS shell");
    append_line(shell, "backend: minimal embedded recovery fallback");
    append_line(shell, "PTY and child shell launch failed");
    append_line(shell, "type 'help' for recovery commands");
    append_line(shell, "");
}

void gui_shell_close(gui_shell_window_t* shell) {
    int pid = shell->pid;

    if (shell->stdin_fd >= 0) {
        sys_close(shell->stdin_fd);
        if (shell->stdout_fd == shell->stdin_fd) shell->stdout_fd = GUI_SHELL_NO_FD;
        shell->stdin_fd = GUI_SHELL_NO_FD;
    }
    if (shell->stdout_fd >= 0) {
        sys_close(shell->stdout_fd);
        shell->stdout_fd = GUI_SHELL_NO_FD;
    }
    if (pid > 0) {
        int status = 0;
        (void)sys_kill(pid, GUI_SHELL_SIGTERM);
        (void)sys_waitpid(pid, &status, 0);
    }
    shell->pid = -1;
}

int gui_shell_poll(gui_shell_window_t* shell) {
    int dirty = 0;
    if (shell->backend != GUI_SHELL_BACKEND_PIPE_CHILD &&
        shell->backend != GUI_SHELL_BACKEND_PTY_CHILD) return 0;

    if (shell->stdout_fd >= 0) {
        char buf[128];
        for (;;) {
            int n = sys_fread(shell->stdout_fd, buf, sizeof(buf) - 1);
            if (n <= 0) break;
            buf[n] = 0;
            print_child_text(shell, buf);
            dirty = 1;
        }
    }

    if (shell->pid > 0) {
        int status = 0;
        int waited = sys_waitpid(shell->pid, &status, SYS_WAITPID_WNOHANG);
        if (waited == shell->pid) {
            if (shell->backend != GUI_SHELL_BACKEND_PTY_CHILD) pending_flush(shell);
            append_line(shell, "");
            append_line(shell, "[shell exited]");
            shell->pid = -1;
            if (shell->stdin_fd >= 0) {
                sys_close(shell->stdin_fd);
                if (shell->stdout_fd == shell->stdin_fd) shell->stdout_fd = GUI_SHELL_NO_FD;
                shell->stdin_fd = GUI_SHELL_NO_FD;
            }
            if (shell->stdout_fd >= 0) {
                sys_close(shell->stdout_fd);
                shell->stdout_fd = GUI_SHELL_NO_FD;
            }
            dirty = 1;
        }
    }

    return dirty;
}

gui_shell_key_result_t gui_shell_handle_key(gui_shell_window_t* shell,
                                            unsigned int ascii,
                                            unsigned int key,
                                            unsigned int flags) {
    if (shell->backend == GUI_SHELL_BACKEND_PTY_CHILD) {
        const char* seq = 0;
        char ch = 0;

        if (shell->stdin_fd < 0) return GUI_SHELL_KEY_OK;
        if ((flags & SYS_INPUT_KEY_CTRL) && (key == KEY_C || key == KEY_D || key == KEY_Z)) {
            if (key == KEY_C) ch = 3;
            else if (key == KEY_D) ch = 4;
            else ch = 26;
        } else if (ascii != 0) {
            ch = (char)(ascii & 0xFFu);
        } else {
            switch (key) {
                case KEY_UP:     seq = "\x1b[A"; break;
                case KEY_DOWN:   seq = "\x1b[B"; break;
                case KEY_RIGHT:  seq = "\x1b[C"; break;
                case KEY_LEFT:   seq = "\x1b[D"; break;
                case KEY_HOME:   seq = "\x1b[H"; break;
                case KEY_END:    seq = "\x1b[F"; break;
                case KEY_DELETE: seq = "\x1b[3~"; break;
                default: break;
            }
        }

        if (seq) {
            sys_writefd(shell->stdin_fd, seq, (uint32_t)s_len(seq));
        } else if (ch != 0) {
            sys_writefd(shell->stdin_fd, &ch, 1);
        }
        return GUI_SHELL_KEY_OK;
    }

    if (shell->backend == GUI_SHELL_BACKEND_PIPE_CHILD) {
        if (ascii == '\n' || ascii == '\r') {
            if (shell->stdin_fd >= 0) {
                gui_shell_history_add(shell, shell->input);
                shell->history_view = gui_shell_history_available(shell);
                gui_shell_track_cd(shell, shell->input);
                if (shell->pending_len > 0) {
                    s_cat(shell->pending, shell->input, sizeof(shell->pending));
                    append_line(shell, shell->pending);
                    pending_clear(shell);
                } else {
                    append_line(shell, shell->input);
                }
                sys_writefd(shell->stdin_fd, shell->input, (uint32_t)s_len(shell->input));
                sys_writefd(shell->stdin_fd, "\n", 1);
                shell->input[0] = 0;
                shell->input_len = 0;
            }
            return GUI_SHELL_KEY_OK;
        }
        if (ascii == '\t') {
            gui_shell_complete(shell);
            shell->history_view = gui_shell_history_available(shell);
            return GUI_SHELL_KEY_OK;
        }
        if (key == KEY_UP) {
            unsigned int avail = gui_shell_history_available(shell);
            const char* hist;
            if (avail == 0 || shell->history_view == 0) return GUI_SHELL_KEY_OK;
            if (shell->history_view == avail) s_copy(shell->saved_input, shell->input, sizeof(shell->saved_input));
            shell->history_view--;
            hist = gui_shell_history_at(shell, shell->history_view);
            gui_shell_set_input(shell, hist);
            return GUI_SHELL_KEY_OK;
        }
        if (key == KEY_DOWN) {
            unsigned int avail = gui_shell_history_available(shell);
            if (avail == 0 || shell->history_view == avail) return GUI_SHELL_KEY_OK;
            shell->history_view++;
            if (shell->history_view == avail) gui_shell_set_input(shell, shell->saved_input);
            else gui_shell_set_input(shell, gui_shell_history_at(shell, shell->history_view));
            return GUI_SHELL_KEY_OK;
        }
        if (ascii == 0x08 || ascii == 0x7F) {
            if (shell->input_len > 0) {
                shell->input_len--;
                shell->input[shell->input_len] = 0;
            }
            shell->history_view = gui_shell_history_available(shell);
            return GUI_SHELL_KEY_OK;
        }
        if (ascii >= 32 && ascii < 127 && shell->input_len + 1 < GUI_SHELL_INPUT) {
            shell->input[shell->input_len++] = (char)ascii;
            shell->input[shell->input_len] = 0;
            shell->history_view = gui_shell_history_available(shell);
        }
        return GUI_SHELL_KEY_OK;
    }

    if (ascii == '\n' || ascii == '\r') {
        gui_shell_history_add(shell, shell->input);
        shell->history_view = gui_shell_history_available(shell);
        return run_command(shell);
    }
    if (ascii == '\t') {
        gui_shell_complete(shell);
        shell->history_view = gui_shell_history_available(shell);
        return GUI_SHELL_KEY_OK;
    }
    if (key == KEY_UP) {
        unsigned int avail = gui_shell_history_available(shell);
        const char* hist;
        if (avail == 0 || shell->history_view == 0) return GUI_SHELL_KEY_OK;
        if (shell->history_view == avail) s_copy(shell->saved_input, shell->input, sizeof(shell->saved_input));
        shell->history_view--;
        hist = gui_shell_history_at(shell, shell->history_view);
        gui_shell_set_input(shell, hist);
        return GUI_SHELL_KEY_OK;
    }
    if (key == KEY_DOWN) {
        unsigned int avail = gui_shell_history_available(shell);
        if (avail == 0 || shell->history_view == avail) return GUI_SHELL_KEY_OK;
        shell->history_view++;
        if (shell->history_view == avail) gui_shell_set_input(shell, shell->saved_input);
        else gui_shell_set_input(shell, gui_shell_history_at(shell, shell->history_view));
        return GUI_SHELL_KEY_OK;
    }
    if (ascii == 0x08 || ascii == 0x7F) {
        if (shell->input_len > 0) {
            shell->input_len--;
            shell->input[shell->input_len] = 0;
        }
        shell->history_view = gui_shell_history_available(shell);
        return GUI_SHELL_KEY_OK;
    }
    if (ascii >= 32 && ascii < 127 && shell->input_len + 1 < GUI_SHELL_INPUT) {
        shell->input[shell->input_len++] = (char)ascii;
        shell->input[shell->input_len] = 0;
        shell->history_view = gui_shell_history_available(shell);
    }
    return GUI_SHELL_KEY_OK;
}
