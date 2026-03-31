#include "line_editor.h"

void line_editor_init(line_editor_t* ed) {
    ed->len = 0;
    ed->cursor = 0;
    ed->buf[0] = '\0';
}

void line_editor_clear(line_editor_t* ed) {
    ed->len = 0;
    ed->cursor = 0;
    ed->buf[0] = '\0';
}

int line_editor_insert(line_editor_t* ed, char c) {
    if (ed->len >= LINE_EDITOR_MAX - 1) {
        return 0;
    }

    for (int i = ed->len; i > ed->cursor; i--) {
        ed->buf[i] = ed->buf[i - 1];
    }

    ed->buf[ed->cursor] = c;
    ed->len++;
    ed->cursor++;
    ed->buf[ed->len] = '\0';
    return 1;
}

int line_editor_backspace(line_editor_t* ed) {
    if (ed->cursor == 0 || ed->len == 0) {
        return 0;
    }

    for (int i = ed->cursor - 1; i < ed->len - 1; i++) {
        ed->buf[i] = ed->buf[i + 1];
    }

    ed->len--;
    ed->cursor--;
    ed->buf[ed->len] = '\0';
    return 1;
}

int line_editor_delete(line_editor_t* ed) {
    if (ed->cursor >= ed->len || ed->len == 0) {
        return 0;
    }

    for (int i = ed->cursor; i < ed->len - 1; i++) {
        ed->buf[i] = ed->buf[i + 1];
    }

    ed->len--;
    ed->buf[ed->len] = '\0';
    return 1;
}

void line_editor_move_left(line_editor_t* ed) {
    if (ed->cursor > 0) {
        ed->cursor--;
    }
}

void line_editor_move_right(line_editor_t* ed) {
    if (ed->cursor < ed->len) {
        ed->cursor++;
    }
}

void line_editor_move_home(line_editor_t* ed) {
    ed->cursor = 0;
}

void line_editor_move_end(line_editor_t* ed) {
    ed->cursor = ed->len;
}