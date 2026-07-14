#include "native_apps.h"
#include "framework_internal.h"

#include "network_model.h"
#include "tasks_model.h"
#include "viewer_model.h"
#include "../image_bmp.h"
#include "keyboard.h"
#include "signal.h"
#include "unistd.h"
#include "user_lib.h"

#define TASK_ROW_H 13
#define TASK_MAX SYS_PROCINFO_MAX

static gui_native_ui_t g_ui;
static char g_pref_address[32];
static char g_pref_prefix[8];
static char g_pref_gateway[32];
static char g_pref_dns[32];

static int inside(int x, int y, gui_rect_t r) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static void copy_text(char* dst, const char* src, unsigned int cap) {
    unsigned int i = 0;
    if (!cap) return;
    while (src && src[i] && i + 1u < cap) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void append_text(char* dst, const char* src, unsigned int cap) {
    unsigned int n = 0;
    while (n < cap && dst[n]) n++;
    while (src && *src && n + 1u < cap) dst[n++] = *src++;
    if (n < cap) dst[n] = 0;
}

static void uint_text(unsigned int value, char* out) {
    char reverse[16];
    int n = 0;
    if (!value) { out[0] = '0'; out[1] = 0; return; }
    while (value && n < 16) { reverse[n++] = (char)('0' + value % 10u); value /= 10u; }
    for (int i = 0; i < n; i++) out[i] = reverse[n - i - 1];
    out[n] = 0;
}

static void draw_value(gfx_surface_t* s, int x, int y, unsigned int value) {
    char text[16];
    uint_text(value, text);
    g_ui.draw_text(s, x, y, text, g_ui.text);
}

/* ---------------- Viewer ---------------- */

typedef struct viewer_state {
    unsigned char* data;
    unsigned int size;
    bmp_image_t bmp;
    gfx_surface_t cache;
    int cache_w;
    int cache_h;
    int zoom_percent;
    int pan_x;
    int pan_y;
    int dragging;
    int drag_x;
    int drag_y;
    int loaded;
    char path[256];
    char status[80];
} viewer_state_t;

static int viewer_read(viewer_state_t* state, const char* path) {
    sys_stat_info_t info;
    int fd;
    unsigned int pos = 0;
    if (!path || !path[0] || sys_stat_full(path, &info) < 0 || info.size == 0u) {
        copy_text(state->status, "Choose a BMP from Files", sizeof(state->status));
        return 0;
    }
    state->data = malloc(info.size);
    if (!state->data) {
        copy_text(state->status, "Out of memory", sizeof(state->status));
        return 0;
    }
    fd = sys_open(path);
    if (fd < 0) {
        free(state->data); state->data = 0;
        copy_text(state->status, "Could not open image", sizeof(state->status));
        return 0;
    }
    while (pos < info.size) {
        int n = read(fd, state->data + pos, info.size - pos);
        if (n <= 0) break;
        pos += (unsigned int)n;
    }
    close(fd);
    if (pos != info.size || bmp_parse(state->data, info.size, &state->bmp) != BMP_OK) {
        free(state->data); state->data = 0;
        copy_text(state->status, "Unsupported or damaged BMP", sizeof(state->status));
        return 0;
    }
    state->size = info.size;
    state->loaded = 1;
    state->zoom_percent = 0;
    copy_text(state->path, path, sizeof(state->path));
    copy_text(state->status, "Fit", sizeof(state->status));
    return 1;
}

static void viewer_open(gui_app_context_t* context, const char* argument) {
    gui_window_t* window = context->window;
    viewer_state_t* state = window->state;
    memset(state, 0, sizeof(*state));
    if (viewer_read(state, argument)) gui_window_set_title(window, "Viewer");
}

static void viewer_close(gui_app_context_t* context) {
    gui_window_t* window = context->window;
    viewer_state_t* state = window->state;
    gfx_surface_free(&state->cache);
    if (state->data) free(state->data);
}

static void viewer_replace_image(viewer_state_t* state, const char* path) {
    gfx_surface_free(&state->cache);
    if (state->data) free(state->data);
    state->data = 0;
    state->loaded = 0;
    state->cache_w = state->cache_h = 0;
    state->pan_x = state->pan_y = 0;
    (void)viewer_read(state, path);
}

static int viewer_rebuild(viewer_state_t* state, int area_w, int area_h) {
    int scaled_w;
    int scaled_h;
    unsigned int dest_w;
    unsigned int dest_h;
    unsigned int* row;
    if (!state->loaded || area_w <= 0 || area_h <= 0) return 0;
    gui_viewer_scaled_size(state->bmp.width, state->bmp.height,
                           area_w, area_h, state->zoom_percent,
                           &scaled_w, &scaled_h);
    dest_w = (unsigned int)scaled_w;
    dest_h = (unsigned int)scaled_h;
    if (state->cache.pixels && state->cache_w == (int)dest_w &&
        state->cache_h == (int)dest_h) return 1;
    gfx_surface_free(&state->cache);
    if (!gfx_surface_alloc(&state->cache, dest_w, dest_h)) return 0;
    row = malloc(state->bmp.width * sizeof(unsigned int));
    if (!row) { gfx_surface_free(&state->cache); return 0; }
    for (unsigned int y = 0; y < dest_h; y++) {
        unsigned int source_y = y * state->bmp.height / dest_h;
        if (bmp_decode_row_xrgb8888(&state->bmp, source_y, row,
                                   state->bmp.width) != BMP_OK) {
            free(row); gfx_surface_free(&state->cache); return 0;
        }
        for (unsigned int x = 0; x < dest_w; x++)
            state->cache.pixels[y * state->cache.pitch_pixels + x] =
                row[x * state->bmp.width / dest_w];
    }
    free(row);
    state->cache_w = (int)dest_w;
    state->cache_h = (int)dest_h;
    return 1;
}

static void viewer_clamp(viewer_state_t* state, int area_w, int area_h) {
    gui_viewer_geometry_t geometry = {
        state->cache_w, state->cache_h, state->pan_x, state->pan_y
    };
    gui_viewer_clamp_pan(&geometry, area_w, area_h);
    state->pan_x = geometry.pan_x;
    state->pan_y = geometry.pan_y;
}

static void viewer_draw(gfx_surface_t* s, gui_app_context_t* context,
                        int mx, int my) {
    gui_window_t* window = context->window;
    viewer_state_t* state = window->state;
    int bx = window->x;
    int by = window->y + 18;
    int bw = window->w;
    int bh = window->h - 18;
    int area_y = by + 24;
    int area_h = bh - 38;
    (void)mx; (void)my;
    gui_canvas_fill_rect(s, bx, by, bw, bh, g_ui.window_bg);
    gui_widget_button(s, gui_rect_make(bx + 4, by + 3, 38, 18), "Open",
                      (gui_widget_state_t){0}, g_ui.widget_theme, g_ui.draw_text);
    gui_widget_button(s, gui_rect_make(bx + 46, by + 3, 38, 18), "Fit",
                      (gui_widget_state_t){0,0,state->zoom_percent == 0,0},
                      g_ui.widget_theme, g_ui.draw_text);
    gui_widget_button(s, gui_rect_make(bx + 88, by + 3, 38, 18), "100%",
                      (gui_widget_state_t){0,0,state->zoom_percent == 100,0},
                      g_ui.widget_theme, g_ui.draw_text);
    gui_widget_button(s, gui_rect_make(bx + 130, by + 3, 24, 18), "-",
                      (gui_widget_state_t){0}, g_ui.widget_theme, g_ui.draw_text);
    gui_widget_button(s, gui_rect_make(bx + 158, by + 3, 24, 18), "+",
                      (gui_widget_state_t){0}, g_ui.widget_theme, g_ui.draw_text);
    gui_canvas_fill_rect(s, bx + 1, area_y, bw - 2, area_h, 0x00202020u);
    if (viewer_rebuild(state, bw - 8, area_h - 4)) {
        viewer_clamp(state, bw - 8, area_h - 4);
        int x0 = bx + (bw - state->cache_w) / 2 + state->pan_x;
        int y0 = area_y + (area_h - state->cache_h) / 2 + state->pan_y;
        int left = x0 < bx + 1 ? bx + 1 : x0;
        int top = y0 < area_y ? area_y : y0;
        int right = x0 + state->cache_w;
        int bottom = y0 + state->cache_h;
        if (right > bx + bw - 1) right = bx + bw - 1;
        if (bottom > area_y + area_h) bottom = area_y + area_h;
        if (left < 0) left = 0;
        if (top < 0) top = 0;
        if (right > (int)s->width) right = (int)s->width;
        if (bottom > (int)s->height) bottom = (int)s->height;
        for (int y = top; y < bottom; y++) {
            unsigned int* dst = s->pixels + y * s->pitch_pixels + left;
            unsigned int* src = state->cache.pixels +
                (y - y0) * state->cache.pitch_pixels + (left - x0);
            for (int x = left; x < right; x++) *dst++ = *src++;
        }
    } else {
        g_ui.draw_text(s, bx + 8, area_y + 10, state->status, g_ui.text);
    }
    g_ui.draw_text(s, bx + 5, by + bh - 10, state->status, g_ui.subtext);
    gui_canvas_rect(s, window->x, window->y, window->w, window->h, g_ui.frame);
}

static unsigned int viewer_event(gui_app_context_t* context,
                                 const gui_app_event_t* event) {
    gui_window_t* window = context->window;
    viewer_state_t* state = window->state;
    if (event->type == GUI_APP_EVENT_FILE_SELECTED) {
        viewer_replace_image(state, event->path);
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_FILE_CANCELLED) {
        return GUI_APP_RESULT_HANDLED;
    }
    if (event->type == GUI_APP_EVENT_RESIZE) {
        state->cache_w = state->cache_h = 0;
        return GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_POINTER_DOWN && event->y < 24) {
        if (event->x >= 4 && event->x < 42) {
            (void)gui_app_open_file_picker(context, GUI_FILE_REQUEST_OPEN,
                                           GUI_FILE_FILTER_BMP,
                                           state->path[0] ? state->path : "/");
            return GUI_APP_RESULT_HANDLED;
        } else if (event->x >= 46 && event->x < 84) state->zoom_percent = 0;
        else if (event->x >= 88 && event->x < 126) state->zoom_percent = 100;
        else if (event->x >= 130 && event->x < 154) {
            if (!state->zoom_percent) state->zoom_percent = 100;
            state->zoom_percent -= 25;
            if (state->zoom_percent < 25) state->zoom_percent = 25;
        } else if (event->x >= 158 && event->x < 182) {
            if (!state->zoom_percent) state->zoom_percent = 100;
            state->zoom_percent += 25;
            if (state->zoom_percent > 400) state->zoom_percent = 400;
        } else return GUI_APP_RESULT_NONE;
        state->cache_w = state->cache_h = 0;
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_POINTER_DOWN && state->loaded) {
        state->dragging = 1;
        state->drag_x = event->x;
        state->drag_y = event->y;
        return GUI_APP_RESULT_HANDLED;
    }
    if (event->type == GUI_APP_EVENT_POINTER_MOVE && state->dragging) {
        state->pan_x += event->x - state->drag_x;
        state->pan_y += event->y - state->drag_y;
        state->drag_x = event->x;
        state->drag_y = event->y;
        gui_app_invalidate(context, 0, 24, window->w, window->h - 38);
        return GUI_APP_RESULT_HANDLED;
    }
    if (event->type == GUI_APP_EVENT_POINTER_UP && state->dragging) {
        state->dragging = 0;
        return GUI_APP_RESULT_HANDLED;
    }
    if (event->type == GUI_APP_EVENT_KEY) {
        if (event->ascii == 'o' || event->ascii == 'O') {
            (void)gui_app_open_file_picker(context, GUI_FILE_REQUEST_OPEN,
                                           GUI_FILE_FILTER_BMP,
                                           state->path[0] ? state->path : "/");
            return GUI_APP_RESULT_HANDLED;
        }
        if (event->ascii == 'f' || event->ascii == 'F') {
            state->zoom_percent = 0;
            state->cache_w = state->cache_h = 0;
        } else if (event->ascii == '0') {
            state->zoom_percent = 100;
            state->cache_w = state->cache_h = 0;
        } else if (event->key == KEY_EQUALS || event->ascii == '+') {
            if (!state->zoom_percent) state->zoom_percent = 100;
            if (state->zoom_percent < 400) state->zoom_percent += 25;
            state->cache_w = state->cache_h = 0;
        } else if (event->key == KEY_MINUS || event->ascii == '-') {
            if (!state->zoom_percent) state->zoom_percent = 100;
            if (state->zoom_percent > 25) state->zoom_percent -= 25;
            state->cache_w = state->cache_h = 0;
        } else if (event->key == KEY_LEFT) state->pan_x += 12;
        else if (event->key == KEY_RIGHT) state->pan_x -= 12;
        else if (event->key == KEY_UP) state->pan_y += 12;
        else if (event->key == KEY_DOWN) state->pan_y -= 12;
        else return GUI_APP_RESULT_NONE;
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    return GUI_APP_RESULT_NONE;
}

/* ---------------- Tasks ---------------- */

typedef struct task_row {
    sys_procinfo_entry_t entry;
    unsigned int cpu_delta;
    unsigned int cpu_percent;
} task_row_t;

typedef struct tasks_state {
    sys_procinfo_t previous;
    task_row_t rows[TASK_MAX];
    unsigned int row_count;
    int have_previous;
    int selection;
    int confirm;
    int sort_column;
    int sort_descending;
    char status[80];
} tasks_state_t;

static int native_text_compare(const char* a, const char* b) {
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    return *a ? 1 : *b ? -1 : 0;
}

static int tasks_compare(const task_row_t* a, const task_row_t* b,
                         int column) {
    gui_task_model_row_t left = {
        a->entry.pid, a->entry.parent_pid, a->entry.state, a->cpu_delta,
        a->entry.ram_bytes, a->entry.heap_bytes, a->entry.name
    };
    gui_task_model_row_t right = {
        b->entry.pid, b->entry.parent_pid, b->entry.state, b->cpu_delta,
        b->entry.ram_bytes, b->entry.heap_bytes, b->entry.name
    };
    return gui_tasks_compare(&left, &right, column);
}

static void tasks_refresh(tasks_state_t* state) {
    sys_procinfo_t current;
    unsigned int elapsed;
    unsigned int selected_pid = 0;
    if (state->row_count && state->selection >= 0 &&
        state->selection < (int)state->row_count)
        selected_pid = state->rows[state->selection].entry.pid;
    if (sys_procinfo(&current) < 0) {
        copy_text(state->status, "Process information unavailable", sizeof(state->status));
        return;
    }
    elapsed = state->have_previous && current.total_ticks > state->previous.total_ticks
            ? current.total_ticks - state->previous.total_ticks : current.total_ticks;
    if (!elapsed) elapsed = 1;
    state->row_count = current.out_count;
    for (unsigned int i = 0; i < current.out_count; i++) {
        unsigned int old_cpu = 0;
        state->rows[i].entry = current.entries[i];
        if (state->have_previous) {
            for (unsigned int j = 0; j < state->previous.out_count; j++)
                if (state->previous.entries[j].pid == current.entries[i].pid)
                    old_cpu = state->previous.entries[j].cpu_ticks;
        }
        state->rows[i].cpu_delta = current.entries[i].cpu_ticks >= old_cpu
            ? current.entries[i].cpu_ticks - old_cpu : current.entries[i].cpu_ticks;
        state->rows[i].cpu_percent = state->rows[i].cpu_delta * 100u / elapsed;
    }
    for (unsigned int i = 0; i < state->row_count; i++)
        for (unsigned int j = i + 1; j < state->row_count; j++)
            if (tasks_compare(&state->rows[j], &state->rows[i],
                              state->sort_column) != 0 &&
                ((tasks_compare(&state->rows[j], &state->rows[i],
                                state->sort_column) > 0) ==
                 state->sort_descending)) {
                task_row_t swap = state->rows[i]; state->rows[i] = state->rows[j];
                state->rows[j] = swap;
            }
    state->previous = current;
    state->have_previous = 1;
    if (selected_pid) {
        for (unsigned int i = 0; i < state->row_count; i++)
            if (state->rows[i].entry.pid == selected_pid) {
                state->selection = (int)i;
                selected_pid = 0;
                break;
            }
    }
    if (selected_pid || state->selection >= (int)state->row_count)
        state->selection = state->row_count ? (int)state->row_count - 1 : 0;
}

static void tasks_open(gui_app_context_t* context, const char* argument) {
    gui_window_t* window = context->window;
    tasks_state_t* state = window->state;
    (void)argument;
    memset(state, 0, sizeof(*state));
    state->sort_column = 3;
    state->sort_descending = 1;
    tasks_refresh(state);
}

static int tasks_protected(const task_row_t* row) {
    gui_task_model_row_t model;
    if (!row) return 1;
    model.pid = row->entry.pid;
    model.parent_pid = row->entry.parent_pid;
    model.state = row->entry.state;
    model.cpu_delta = row->cpu_delta;
    model.ram_bytes = row->entry.ram_bytes;
    model.heap_bytes = row->entry.heap_bytes;
    model.name = row->entry.name;
    return gui_tasks_is_protected(&model, (unsigned int)sys_getpid());
}

static void tasks_draw(gfx_surface_t* s, gui_app_context_t* context,
                       int mx, int my) {
    gui_window_t* window = context->window;
    tasks_state_t* state = window->state;
    int bx = window->x, by = window->y + 18;
    int bh = window->h - 18;
    int visible = (bh - 48) / TASK_ROW_H;
    (void)mx; (void)my;
    gui_canvas_fill_rect(s, bx, by, window->w, bh, g_ui.window_bg);
    {
        gui_table_column_t columns[] = {
            {"PID", 42}, {"NAME", 112}, {"STATE", 48},
            {"CPU", 42}, {"RAM", 54}, {"HEAP", 54}
        };
        gui_widget_table_header(s, gui_rect_make(bx + 2, by + 2,
                                window->w - 4, 14), columns, 6,
                                state->sort_column, state->sort_descending,
                                g_ui.widget_theme, g_ui.draw_text);
    }
    for (int i = 0; i < visible && i < (int)state->row_count; i++) {
        int y = by + 17 + i * TASK_ROW_H;
        task_row_t* row = &state->rows[i];
        if (i == state->selection)
            gui_canvas_fill_rect(s, bx + 2, y, window->w - 4, TASK_ROW_H,
                                 g_ui.highlight);
        unsigned int color = i == state->selection ? g_ui.highlight_text : g_ui.text;
        char n[16];
        uint_text(row->entry.pid, n);
        g_ui.draw_text(s, bx + 5, y + 3, n, color);
        g_ui.draw_text(s, bx + 45, y + 3, row->entry.name, color);
        uint_text(row->entry.state, n);
        g_ui.draw_text(s, bx + 157, y + 3, n, color);
        uint_text(row->cpu_percent, n);
        g_ui.draw_text(s, bx + 205, y + 3, n, color);
        uint_text(row->entry.ram_bytes / 1024u, n);
        g_ui.draw_text(s, bx + 247, y + 3, n, color);
        uint_text(row->entry.heap_bytes / 1024u, n);
        g_ui.draw_text(s, bx + 301, y + 3, n, color);
    }
    gui_widget_button(s, gui_rect_make(bx + 4, by + bh - 25, 76, 18), "Terminate",
                      (gui_widget_state_t){0,0,0,state->row_count == 0 ||
                          tasks_protected(&state->rows[state->selection])},
                      g_ui.widget_theme, g_ui.draw_text);
    g_ui.draw_text(s, bx + 86, by + bh - 19, state->status, g_ui.subtext);
    if (state->confirm) {
        int dx = bx + (window->w - 220) / 2, dy = by + (bh - 76) / 2;
        gui_canvas_fill_rect(s, dx, dy, 220, 76, g_ui.window_bg);
        gui_canvas_rect(s, dx, dy, 220, 76, g_ui.frame);
        g_ui.draw_text(s, dx + 8, dy + 12, "Terminate selected process?", g_ui.text);
        gui_widget_button(s, gui_rect_make(dx + 8, dy + 44, 90, 20), "Terminate",
                          (gui_widget_state_t){0}, g_ui.widget_theme, g_ui.draw_text);
        gui_widget_button(s, gui_rect_make(dx + 118, dy + 44, 90, 20), "Cancel",
                          (gui_widget_state_t){0}, g_ui.widget_theme, g_ui.draw_text);
    }
    gui_canvas_rect(s, window->x, window->y, window->w, window->h, g_ui.frame);
}

static unsigned int tasks_event(gui_app_context_t* context,
                                const gui_app_event_t* event) {
    gui_window_t* window = context->window;
    tasks_state_t* state = window->state;
    int bh = window->h - 18;
    if (event->type == GUI_APP_EVENT_TICK) {
        tasks_refresh(state);
        gui_app_invalidate(context, 0, 0, window->w, window->h - 18);
        return GUI_APP_RESULT_HANDLED;
    }
    if (event->type == GUI_APP_EVENT_POINTER_DOWN) {
        if (state->confirm) {
            int dx = (window->w - 220) / 2, dy = (bh - 76) / 2;
            if (inside(event->x, event->y, gui_rect_make(dx + 8, dy + 44, 90, 20))) {
                task_row_t selected = state->rows[state->selection];
                if (sys_kill((int)selected.entry.pid, SIGTERM) < 0)
                    copy_text(state->status, "Terminate failed", sizeof(state->status));
                else copy_text(state->status, "SIGTERM sent", sizeof(state->status));
                state->confirm = 0;
            } else if (inside(event->x, event->y,
                              gui_rect_make(dx + 118, dy + 44, 90, 20))) {
                state->confirm = 0;
            }
            return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
        }
        if (event->y >= 2 && event->y < 16) {
            static const int edges[] = {42, 154, 202, 244, 298, 352};
            int column = 5;
            for (int i = 0; i < 6; i++)
                if (event->x - 2 < edges[i]) { column = i; break; }
            if (state->sort_column == column)
                state->sort_descending = !state->sort_descending;
            else {
                state->sort_column = column;
                state->sort_descending = column != 0 && column != 1;
            }
            tasks_refresh(state);
            return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
        }
        if (event->y >= 17 && event->y < bh - 30) {
            int row = (event->y - 17) / TASK_ROW_H;
            if (row >= 0 && row < (int)state->row_count) state->selection = row;
            return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
        }
        if (inside(event->x, event->y, gui_rect_make(4, bh - 25, 76, 18)) &&
            state->row_count && !tasks_protected(&state->rows[state->selection])) {
            state->confirm = 1;
            return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
        }
    }
    if (event->type == GUI_APP_EVENT_KEY) {
        if (event->key == KEY_UP && state->selection > 0) state->selection--;
        else if (event->key == KEY_DOWN && state->selection + 1 < (int)state->row_count)
            state->selection++;
        else if ((event->key == KEY_ENTER || event->key == KEY_DELETE) &&
                 state->row_count && !tasks_protected(&state->rows[state->selection]))
            state->confirm = 1;
        else if (event->key == KEY_ESC && state->confirm) state->confirm = 0;
        else return GUI_APP_RESULT_NONE;
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    return GUI_APP_RESULT_NONE;
}

/* ---------------- Network ---------------- */

typedef struct network_state {
    sys_netinfo_t info;
    gui_text_input_t ip;
    gui_text_input_t prefix;
    gui_text_input_t gateway;
    gui_text_input_t dns;
    int focus;
    char status[80];
} network_state_t;

static void ip_text(unsigned int ip, char* out) {
    char n[16]; out[0] = 0;
    for (int shift = 24; shift >= 0; shift -= 8) {
        uint_text((ip >> shift) & 255u, n); append_text(out, n, 32);
        if (shift) append_text(out, ".", 32);
    }
}

static void network_load(network_state_t* state) {
    char text[32], number[16];
    gui_text_input_init(&state->ip, "");
    gui_text_input_init(&state->prefix, "");
    gui_text_input_init(&state->gateway, "");
    gui_text_input_init(&state->dns, "");
    if (sys_netinfo(&state->info) < 0) {
        copy_text(state->status, "Network information unavailable", sizeof(state->status));
        return;
    }
    if (state->info.ipv4_configured) {
        ip_text(state->info.ip, text); gui_text_input_init(&state->ip, text);
        unsigned int mask = state->info.netmask, prefix = 0;
        if (!gui_ipv4_mask_prefix(mask, &prefix)) {
            copy_text(state->status, "Invalid noncontiguous netmask",
                      sizeof(state->status));
        } else {
            uint_text(prefix, number); gui_text_input_init(&state->prefix, number);
        }
        if (state->info.gateway) ip_text(state->info.gateway, text); else text[0] = 0;
        gui_text_input_init(&state->gateway, text);
        if (state->info.dns) ip_text(state->info.dns, text); else text[0] = 0;
        gui_text_input_init(&state->dns, text);
    }
}

static void network_open(gui_app_context_t* context, const char* argument) {
    gui_window_t* window = context->window;
    network_state_t* state = window->state; (void)argument;
    memset(state, 0, sizeof(*state)); network_load(state);
    if (!state->info.ipv4_configured && g_pref_address[0]) {
        gui_text_input_init(&state->ip, g_pref_address);
        gui_text_input_init(&state->prefix, g_pref_prefix);
        gui_text_input_init(&state->gateway, g_pref_gateway);
        gui_text_input_init(&state->dns, g_pref_dns);
    }
}

static gui_rect_t network_field(int index) {
    return gui_rect_make(78, 86 + index * 24, index == 1 ? 48 : 142, 18);
}

static void network_draw(gfx_surface_t* s, gui_app_context_t* context,
                         int mx, int my) {
    gui_window_t* window = context->window;
    network_state_t* state = window->state;
    int bx = window->x, by = window->y + 18, bh = window->h - 18;
    char text[96], n[16], ip[32]; (void)mx; (void)my;
    gui_canvas_fill_rect(s, bx, by, window->w, bh, g_ui.window_bg);
    copy_text(text, "Driver ", sizeof(text)); append_text(text, state->info.net_driver, sizeof(text));
    append_text(text, state->info.net_link_up ? "  Up  MAC " : "  Down  MAC ", sizeof(text));
    for (int i = 0; i < 6; i++) {
        static const char hex[] = "0123456789ABCDEF";
        char byte[4] = {hex[state->info.mac[i] >> 4],
                        hex[state->info.mac[i] & 15u], i == 5 ? 0 : ':', 0};
        append_text(text, byte, sizeof(text));
    }
    g_ui.draw_text(s, bx + 6, by + 7, text, g_ui.text);
    copy_text(text, "Packets TX ", sizeof(text)); uint_text(state->info.nic_tx_packets, n); append_text(text,n,sizeof(text));
    append_text(text, "  RX ", sizeof(text)); uint_text(state->info.nic_rx_packets,n); append_text(text,n,sizeof(text));
    append_text(text, "  Err ", sizeof(text));
    uint_text(state->info.nic_tx_errors + state->info.nic_rx_errors,n); append_text(text,n,sizeof(text));
    g_ui.draw_text(s, bx + 6, by + 22, text, g_ui.subtext);
    copy_text(text, "Sockets ", sizeof(text)); uint_text(state->info.used_sockets,n); append_text(text,n,sizeof(text));
    append_text(text,"/",sizeof(text)); uint_text(state->info.max_sockets,n); append_text(text,n,sizeof(text));
    append_text(text, "  DHCP ", sizeof(text));
    if (state->info.dhcp_server) { ip_text(state->info.dhcp_server, ip); append_text(text,ip,sizeof(text)); }
    else append_text(text,"none",sizeof(text));
    append_text(text, "  Lease ", sizeof(text)); uint_text(state->info.lease_seconds,n); append_text(text,n,sizeof(text));
    g_ui.draw_text(s, bx + 6, by + 37, text, g_ui.subtext);
    if (state->info.ipv4_configured) {
        ip_text(state->info.ip, ip); copy_text(text,"Current ",sizeof(text)); append_text(text,ip,sizeof(text));
        append_text(text,"  GW ",sizeof(text));
        if (state->info.gateway) { ip_text(state->info.gateway,ip); append_text(text,ip,sizeof(text)); }
        else append_text(text,"none",sizeof(text));
        append_text(text,"  DNS ",sizeof(text));
        if (state->info.dns) { ip_text(state->info.dns,ip); append_text(text,ip,sizeof(text)); }
        else append_text(text,"none",sizeof(text));
    }
    else copy_text(text, "Current unconfigured", sizeof(text));
    g_ui.draw_text(s, bx + 6, by + 54, text, g_ui.text);
    const char* labels[] = {"Address", "Prefix", "Gateway", "DNS"};
    gui_text_input_t* inputs[] = {&state->ip,&state->prefix,&state->gateway,&state->dns};
    for (int i = 0; i < 4; i++) {
        gui_rect_t f = network_field(i); f.x += bx; f.y += by;
        g_ui.draw_text(s, bx + 6, by + 92 + i * 24, labels[i], g_ui.text);
        gui_widget_text_field(s, f, inputs[i]->text, inputs[i]->cursor,
                              (gui_widget_state_t){0,0,state->focus == i + 1,0},
                              g_ui.widget_theme, g_ui.draw_text);
    }
    gui_widget_button(s, gui_rect_make(bx + 6, by + 186, 64, 20), "Apply",
                      (gui_widget_state_t){0},g_ui.widget_theme,g_ui.draw_text);
    gui_widget_button(s, gui_rect_make(bx + 76, by + 186, 72, 20), "DHCP",
                      (gui_widget_state_t){0},g_ui.widget_theme,g_ui.draw_text);
    gui_widget_button(s, gui_rect_make(bx + 154, by + 186, 72, 20), "Release",
                      (gui_widget_state_t){0},g_ui.widget_theme,g_ui.draw_text);
    g_ui.draw_text(s, bx + 6, by + bh - 12, state->status, g_ui.subtext);
    gui_canvas_rect(s, window->x, window->y, window->w, window->h, g_ui.frame);
}

static int network_apply(network_state_t* state) {
    gui_ipv4_config_t config;
    gui_ipv4_validation_t validation;
    sys_net_op_request_t request;
    validation = gui_ipv4_validate(state->ip.text, state->prefix.text,
                                   state->gateway.text, state->dns.text,
                                   &config);
    if (validation == GUI_IPV4_NETWORK_OR_BROADCAST)
        copy_text(state->status, "Address is network/broadcast", sizeof(state->status));
    else if (validation == GUI_IPV4_GATEWAY_OUTSIDE_SUBNET)
        copy_text(state->status, "Gateway is outside subnet", sizeof(state->status));
    else if (validation != GUI_IPV4_VALID)
        copy_text(state->status, "Invalid IPv4 configuration", sizeof(state->status));
    if (validation != GUI_IPV4_VALID) return 0;
    memset(&request, 0, sizeof(request)); request.op = SYS_NET_OP_CONFIGURE;
    request.target_ip = config.address; request.netmask = config.netmask;
    request.gateway = config.gateway; request.dns = config.dns;
    if (sys_net_op(&request) <= 0) { copy_text(state->status,"Apply failed",sizeof(state->status)); return 0; }
    copy_text(g_pref_address, state->ip.text, sizeof(g_pref_address));
    copy_text(g_pref_prefix, state->prefix.text, sizeof(g_pref_prefix));
    copy_text(g_pref_gateway, state->gateway.text, sizeof(g_pref_gateway));
    copy_text(g_pref_dns, state->dns.text, sizeof(g_pref_dns));
    gui_preferences_save();
    copy_text(state->status,"Static configuration applied",sizeof(state->status)); network_load(state); return 1;
}

static unsigned int network_event(gui_app_context_t* context,
                                  const gui_app_event_t* event) {
    gui_window_t* window = context->window;
    network_state_t* state = window->state;
    if (event->type == GUI_APP_EVENT_TICK) {
        sys_netinfo(&state->info);
        gui_app_invalidate(context, 0, 0, window->w, 74);
        return GUI_APP_RESULT_HANDLED;
    }
    if (event->type == GUI_APP_EVENT_POINTER_DOWN) {
        for (int i = 0; i < 4; i++) if (inside(event->x,event->y,network_field(i))) {
            state->focus = i + 1; return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
        }
        if (inside(event->x,event->y,gui_rect_make(6,186,64,20))) network_apply(state);
        else if (inside(event->x,event->y,gui_rect_make(76,186,72,20))) {
            sys_net_op_request_t request; memset(&request,0,sizeof(request)); request.op=SYS_NET_OP_DHCP;
            copy_text(state->status,sys_net_op(&request)>0?"DHCP lease acquired":"DHCP failed",sizeof(state->status)); network_load(state);
        } else if (inside(event->x,event->y,gui_rect_make(154,186,72,20))) {
            sys_net_op_request_t request; memset(&request,0,sizeof(request)); request.op=SYS_NET_OP_CLEAR_CONFIG;
            copy_text(state->status,sys_net_op(&request)>0?"Configuration cleared":"Release failed",sizeof(state->status)); network_load(state);
        } else return GUI_APP_RESULT_NONE;
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_KEY && event->key == KEY_TAB) {
        if (event->modifiers & SYS_INPUT_KEY_SHIFT)
            state->focus = state->focus <= 1 ? 4 : state->focus - 1;
        else
            state->focus = state->focus >= 4 ? 1 : state->focus + 1;
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_KEY && state->focus) {
        gui_text_input_t* inputs[] = {&state->ip,&state->prefix,&state->gateway,&state->dns};
        gui_text_input_t* input = inputs[state->focus - 1]; int changed = 0;
        if (event->key == KEY_LEFT) changed=gui_text_input_command(input,GUI_TEXT_INPUT_LEFT);
        else if (event->key == KEY_RIGHT) changed=gui_text_input_command(input,GUI_TEXT_INPUT_RIGHT);
        else if (event->key == KEY_HOME) changed=gui_text_input_command(input,GUI_TEXT_INPUT_HOME);
        else if (event->key == KEY_END) changed=gui_text_input_command(input,GUI_TEXT_INPUT_END);
        else if (event->key == KEY_BACKSPACE) changed=gui_text_input_command(input,GUI_TEXT_INPUT_BACKSPACE);
        else if (event->key == KEY_DELETE) changed=gui_text_input_command(input,GUI_TEXT_INPUT_DELETE);
        else if (event->key == KEY_ENTER) changed=network_apply(state);
        else if ((event->ascii >= '0' && event->ascii <= '9') || event->ascii == '.') changed=gui_text_input_insert(input,(char)event->ascii);
        else return GUI_APP_RESULT_HANDLED;
        return GUI_APP_RESULT_HANDLED | (changed ? GUI_APP_RESULT_REDRAW : 0);
    }
    return GUI_APP_RESULT_NONE;
}

static const gui_app_descriptor_t VIEWER_DESCRIPTOR = {
    "Viewer", sizeof(viewer_state_t), 560, 400, 280, 180, 0,
    viewer_open, viewer_close, viewer_draw, viewer_event, GUI_APP_VIEWER, 0,
    "viewer", "Viewer", 3, 1
};
static const gui_app_descriptor_t TASKS_DESCRIPTOR = {
    "Tasks", sizeof(tasks_state_t), 520, 360, 360, 220, SMALLOS_TIMER_HZ,
    tasks_open, 0, tasks_draw, tasks_event, GUI_APP_TASKS, 0,
    "tasks", "Tasks", 4, 1
};
static const gui_app_descriptor_t NETWORK_DESCRIPTOR = {
    "Network", sizeof(network_state_t), 420, 270, 320, 250, SMALLOS_TIMER_HZ,
    network_open, 0, network_draw, network_event, GUI_APP_NETWORK, 0,
    "network", "Network", 5, 1
};

void gui_native_apps_init(const gui_native_ui_t* ui) {
    if (ui) g_ui = *ui;
}

const gui_app_descriptor_t* gui_native_app_descriptor(gui_app_id_t id) {
    if (id == GUI_APP_VIEWER) return &VIEWER_DESCRIPTOR;
    if (id == GUI_APP_TASKS) return &TASKS_DESCRIPTOR;
    if (id == GUI_APP_NETWORK) return &NETWORK_DESCRIPTOR;
    return 0;
}

void gui_native_network_pref_set(const char* key, const char* value) {
    if (!key) return;
    if (!native_text_compare(key, "network_address"))
        copy_text(g_pref_address, value, sizeof(g_pref_address));
    else if (!native_text_compare(key, "network_prefix"))
        copy_text(g_pref_prefix, value, sizeof(g_pref_prefix));
    else if (!native_text_compare(key, "network_gateway"))
        copy_text(g_pref_gateway, value, sizeof(g_pref_gateway));
    else if (!native_text_compare(key, "network_dns"))
        copy_text(g_pref_dns, value, sizeof(g_pref_dns));
}

const char* gui_native_network_pref_get(const char* key) {
    if (!key) return "";
    if (!native_text_compare(key, "network_address")) return g_pref_address;
    if (!native_text_compare(key, "network_prefix")) return g_pref_prefix;
    if (!native_text_compare(key, "network_gateway")) return g_pref_gateway;
    if (!native_text_compare(key, "network_dns")) return g_pref_dns;
    return "";
}
