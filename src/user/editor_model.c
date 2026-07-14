#include "editor_model.h"
#include "user_lib.h"

static void copy_string(char* out, const char* in, uint32_t cap) {
    uint32_t i = 0;
    if (!out || cap == 0u) return;
    while (in && in[i] && i + 1u < cap) { out[i] = in[i]; i++; }
    out[i] = '\0';
}

static char* alloc_line(const char* text) {
    char* line = (char*)malloc(EDITOR_LINE_MAX);
    if (!line) return 0;
    copy_string(line, text, EDITOR_LINE_MAX);
    return line;
}

static int reserve(editor_model_t* model, uint32_t needed) {
    uint32_t cap;
    char** lines;
    if (needed <= model->cap) return 1;
    cap = model->cap ? model->cap : 16u;
    while (cap < needed) cap *= 2u;
    lines = (char**)realloc(model->lines, cap * sizeof(char*));
    if (!lines) return 0;
    model->lines = lines;
    model->cap = cap;
    return 1;
}

void editor_model_init(editor_model_t* model, const char* path) {
    if (!model) return;
    model->lines = 0;
    model->count = 0;
    model->cap = 0;
    model->dirty = 0;
    copy_string(model->path, path, sizeof(model->path));
}

void editor_model_destroy(editor_model_t* model) {
    if (!model) return;
    for (uint32_t i = 0; i < model->count; i++) free(model->lines[i]);
    free(model->lines);
    model->lines = 0;
    model->count = 0;
    model->cap = 0;
}

int editor_model_insert_line(editor_model_t* model, uint32_t row,
                             const char* text) {
    char* line;
    if (!model || row > model->count || !reserve(model, model->count + 1u))
        return 0;
    line = alloc_line(text);
    if (!line) return 0;
    for (uint32_t i = model->count; i > row; i--)
        model->lines[i] = model->lines[i - 1u];
    model->lines[row] = line;
    model->count++;
    model->dirty = 1;
    return 1;
}

int editor_model_delete_lines(editor_model_t* model, uint32_t first,
                              uint32_t last) {
    uint32_t remove;
    if (!model || first >= model->count || last < first) return 0;
    if (last >= model->count) last = model->count - 1u;
    remove = last - first + 1u;
    for (uint32_t i = first; i <= last; i++) free(model->lines[i]);
    for (uint32_t i = last + 1u; i < model->count; i++)
        model->lines[i - remove] = model->lines[i];
    model->count -= remove;
    model->dirty = 1;
    return 1;
}

void editor_model_clear(editor_model_t* model) {
    if (!model) return;
    for (uint32_t i = 0; i < model->count; i++) free(model->lines[i]);
    model->count = 0;
    model->dirty = 1;
}

uint32_t editor_model_line_length(const editor_model_t* model, uint32_t row) {
    return model && row < model->count ? str_len(model->lines[row]) : 0u;
}

int editor_model_ensure_line(editor_model_t* model, uint32_t row) {
    while (model && model->count <= row)
        if (!editor_model_insert_line(model, model->count, "")) return 0;
    return model != 0;
}

int editor_model_insert_char(editor_model_t* model, uint32_t row,
                             uint32_t column, char ch) {
    char* line;
    uint32_t len;
    if (!editor_model_ensure_line(model, row)) return 0;
    line = model->lines[row];
    len = str_len(line);
    if (column > len) column = len;
    if (len + 1u >= EDITOR_LINE_MAX) return 0;
    for (uint32_t i = len + 1u; i > column; i--) line[i] = line[i - 1u];
    line[column] = ch;
    model->dirty = 1;
    return 1;
}

int editor_model_split_line(editor_model_t* model, uint32_t row,
                            uint32_t column) {
    char* line;
    char* tail;
    uint32_t len;
    if (!editor_model_ensure_line(model, row)) return 0;
    line = model->lines[row];
    len = str_len(line);
    if (column > len) column = len;
    tail = alloc_line(line + column);
    if (!tail || !reserve(model, model->count + 1u)) {
        free(tail);
        return 0;
    }
    line[column] = '\0';
    for (uint32_t i = model->count; i > row + 1u; i--)
        model->lines[i] = model->lines[i - 1u];
    model->lines[row + 1u] = tail;
    model->count++;
    model->dirty = 1;
    return 1;
}

int editor_model_join_next(editor_model_t* model, uint32_t row) {
    uint32_t len, next_len;
    if (!model || row + 1u >= model->count) return 0;
    len = str_len(model->lines[row]);
    next_len = str_len(model->lines[row + 1u]);
    if (len + next_len >= EDITOR_LINE_MAX) return 0;
    for (uint32_t i = 0; i <= next_len; i++)
        model->lines[row][len + i] = model->lines[row + 1u][i];
    editor_model_delete_lines(model, row + 1u, row + 1u);
    model->dirty = 1;
    return 1;
}

int editor_model_backspace(editor_model_t* model, uint32_t* row,
                           uint32_t* column) {
    char* line;
    uint32_t len, previous;
    if (!model || !row || !column || model->count == 0) return 0;
    if (*row >= model->count) *row = model->count - 1u;
    line = model->lines[*row];
    len = str_len(line);
    if (*column > len) *column = len;
    if (*column > 0) {
        for (uint32_t i = *column - 1u; i < len; i++) line[i] = line[i + 1u];
        (*column)--;
        model->dirty = 1;
        return 1;
    }
    if (*row == 0) return 0;
    previous = str_len(model->lines[*row - 1u]);
    if (!editor_model_join_next(model, *row - 1u)) return 0;
    (*row)--;
    *column = previous;
    return 1;
}

int editor_model_delete(editor_model_t* model, uint32_t row,
                        uint32_t column) {
    char* line;
    uint32_t len;
    if (!model || row >= model->count) return 0;
    line = model->lines[row];
    len = str_len(line);
    if (column < len) {
        for (uint32_t i = column; i < len; i++) line[i] = line[i + 1u];
        model->dirty = 1;
        return 1;
    }
    return editor_model_join_next(model, row);
}

int editor_model_delete_range(editor_model_t* model,
                              uint32_t first_row, uint32_t first_column,
                              uint32_t last_row, uint32_t last_column) {
    uint32_t first_len, last_len, suffix_len, remove;
    char* first;
    if (!model || first_row > last_row || first_row >= model->count ||
        last_row >= model->count) return 0;
    first_len = str_len(model->lines[first_row]);
    last_len = str_len(model->lines[last_row]);
    if (first_column > first_len) first_column = first_len;
    if (last_column > last_len) last_column = last_len;
    if (first_row == last_row) {
        if (last_column <= first_column) return 0;
        first = model->lines[first_row];
        for (uint32_t i = last_column; i <= first_len; i++)
            first[first_column + i - last_column] = first[i];
        model->dirty = 1;
        return 1;
    }
    suffix_len = last_len - last_column;
    if (first_column + suffix_len >= EDITOR_LINE_MAX) return 0;
    first = model->lines[first_row];
    for (uint32_t i = 0; i <= suffix_len; i++)
        first[first_column + i] = model->lines[last_row][last_column + i];
    remove = last_row - first_row;
    for (uint32_t i = first_row + 1u; i <= last_row; i++)
        free(model->lines[i]);
    for (uint32_t i = last_row + 1u; i < model->count; i++)
        model->lines[i - remove] = model->lines[i];
    model->count -= remove;
    model->dirty = 1;
    return 1;
}

int editor_model_load(editor_model_t* model) {
    uint32_t size = 0;
    int is_dir = 0;
    int fd;
    char line[EDITOR_LINE_MAX];
    char chunk[128];
    uint32_t len = 0;
    if (!model || !model->path[0]) return 0;
    if (u_stat(model->path, &size, &is_dir) < 0) return 1;
    if (is_dir) return 0;
    fd = sys_open(model->path);
    if (fd < 0) return 0;
    for (;;) {
        int n = sys_fread(fd, chunk, sizeof(chunk));
        if (n < 0) { sys_close(fd); return 0; }
        if (n == 0) break;
        for (int i = 0; i < n; i++) {
            char c = chunk[i];
            if (c == '\r') continue;
            if (c == '\n') {
                line[len] = '\0';
                if (!editor_model_insert_line(model, model->count, line)) {
                    sys_close(fd); return 0;
                }
                len = 0;
            } else if (len + 1u < EDITOR_LINE_MAX) line[len++] = c;
        }
    }
    if (len > 0) {
        line[len] = '\0';
        if (!editor_model_insert_line(model, model->count, line)) {
            sys_close(fd); return 0;
        }
    }
    sys_close(fd);
    model->dirty = 0;
    return 1;
}

int editor_model_save(editor_model_t* model) {
    int fd;
    if (!model || !model->path[0]) return 0;
    fd = sys_open_mode(model->path,
        SYS_OPEN_MODE_WRITE | SYS_OPEN_MODE_CREATE | SYS_OPEN_MODE_TRUNC);
    if (fd < 0) return 0;
    for (uint32_t i = 0; i < model->count; i++) {
        uint32_t len = str_len(model->lines[i]);
        if ((len && sys_writefd(fd, model->lines[i], len) != (int)len) ||
            sys_writefd(fd, "\n", 1u) != 1) {
            sys_close(fd);
            return 0;
        }
    }
    if (sys_fsync(fd) < 0) { sys_close(fd); return 0; }
    sys_close(fd);
    model->dirty = 0;
    return 1;
}
