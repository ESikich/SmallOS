#include "native_apps_internal.h"

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

enum {
    VIEWER_CONTROL_OPEN = 1,
    VIEWER_CONTROL_FIT,
    VIEWER_CONTROL_100,
    VIEWER_CONTROL_ZOOM_OUT,
    VIEWER_CONTROL_ZOOM_IN,
    VIEWER_CONTROL_CANVAS
};

static gui_rect_t viewer_button_bounds(int control) {
    if (control == VIEWER_CONTROL_OPEN) return gui_rect_make(4, 3, 38, 18);
    if (control == VIEWER_CONTROL_FIT) return gui_rect_make(46, 3, 38, 18);
    if (control == VIEWER_CONTROL_100) return gui_rect_make(88, 3, 38, 18);
    if (control == VIEWER_CONTROL_ZOOM_OUT) return gui_rect_make(130, 3, 24, 18);
    return gui_rect_make(158, 3, 24, 18);
}

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
    viewer_state_t* state = gui_app_state(context);
    memset(state, 0, sizeof(*state));
    if (viewer_read(state, argument)) gui_app_set_title(context, "Viewer");
}

static void viewer_close(gui_app_context_t* context) {
    viewer_state_t* state = gui_app_state(context);
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
    viewer_state_t* state = gui_app_state(context);
    int bx = 0;
    int by = 0;
    int bw = (int)s->width;
    int bh = (int)s->height;
    int area_y = by + 24;
    int area_h = bh - 38;
    int focused = gui_app_focused_control(context);
    int captured = gui_app_captured_control(context);
    (void)mx; (void)my;
    gui_canvas_fill_rect(s, bx, by, bw, bh, g_ui.window_bg);
    gui_widget_button(s, gui_rect_make(bx + 4, by + 3, 38, 18), "Open",
                      (gui_widget_state_t){0,captured == VIEWER_CONTROL_OPEN,
                                           focused == VIEWER_CONTROL_OPEN,0}, g_ui.widget_theme, g_ui.draw_text);
    gui_widget_button(s, gui_rect_make(bx + 46, by + 3, 38, 18), "Fit",
                      (gui_widget_state_t){0,captured == VIEWER_CONTROL_FIT,
                                           focused == VIEWER_CONTROL_FIT || state->zoom_percent == 0,0},
                      g_ui.widget_theme, g_ui.draw_text);
    gui_widget_button(s, gui_rect_make(bx + 88, by + 3, 38, 18), "100%",
                      (gui_widget_state_t){0,captured == VIEWER_CONTROL_100,
                                           focused == VIEWER_CONTROL_100 || state->zoom_percent == 100,0},
                      g_ui.widget_theme, g_ui.draw_text);
    gui_widget_button(s, gui_rect_make(bx + 130, by + 3, 24, 18), "-",
                      (gui_widget_state_t){0,captured == VIEWER_CONTROL_ZOOM_OUT,
                                           focused == VIEWER_CONTROL_ZOOM_OUT,0}, g_ui.widget_theme, g_ui.draw_text);
    gui_widget_button(s, gui_rect_make(bx + 158, by + 3, 24, 18), "+",
                      (gui_widget_state_t){0,captured == VIEWER_CONTROL_ZOOM_IN,
                                           focused == VIEWER_CONTROL_ZOOM_IN,0}, g_ui.widget_theme, g_ui.draw_text);
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
        if (gui_canvas_has_clip()) {
            const gui_rect_t* clip = gui_canvas_clip();
            if (left < clip->x) left = clip->x;
            if (top < clip->y) top = clip->y;
            if (right > clip->x + clip->w) right = clip->x + clip->w;
            if (bottom > clip->y + clip->h) bottom = clip->y + clip->h;
        }
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
}

static unsigned int viewer_event(gui_app_context_t* context,
                                 const gui_app_event_t* event) {
    viewer_state_t* state = gui_app_state(context);
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
        for (int control = VIEWER_CONTROL_OPEN;
             control <= VIEWER_CONTROL_ZOOM_IN; control++) {
            if (gui_widget_hit(viewer_button_bounds(control), event->x, event->y)) {
                gui_app_focus_control(context, control);
                gui_app_capture_pointer(context, control);
                return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
            }
        }
        return GUI_APP_RESULT_NONE;
    }
    if (event->type == GUI_APP_EVENT_POINTER_UP &&
        gui_app_captured_control(context) >= VIEWER_CONTROL_OPEN &&
        gui_app_captured_control(context) <= VIEWER_CONTROL_ZOOM_IN) {
        int control = gui_app_captured_control(context);
        int activate = gui_widget_hit(viewer_button_bounds(control), event->x, event->y);
        gui_app_release_pointer(context);
        if (!activate) return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
        if (control == VIEWER_CONTROL_OPEN) {
            (void)gui_app_open_file_picker(context, GUI_FILE_REQUEST_OPEN,
                                           GUI_FILE_FILTER_BMP,
                                           state->path[0] ? state->path : "/");
            return GUI_APP_RESULT_HANDLED;
        } else if (control == VIEWER_CONTROL_FIT) state->zoom_percent = 0;
        else if (control == VIEWER_CONTROL_100) state->zoom_percent = 100;
        else if (control == VIEWER_CONTROL_ZOOM_OUT) {
            if (!state->zoom_percent) state->zoom_percent = 100;
            state->zoom_percent -= 25;
            if (state->zoom_percent < 25) state->zoom_percent = 25;
        } else if (control == VIEWER_CONTROL_ZOOM_IN) {
            if (!state->zoom_percent) state->zoom_percent = 100;
            state->zoom_percent += 25;
            if (state->zoom_percent > 400) state->zoom_percent = 400;
        }
        state->cache_w = state->cache_h = 0;
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_POINTER_DOWN && state->loaded) {
        state->dragging = 1;
        gui_app_capture_pointer(context, VIEWER_CONTROL_CANVAS);
        state->drag_x = event->x;
        state->drag_y = event->y;
        return GUI_APP_RESULT_HANDLED;
    }
    if (event->type == GUI_APP_EVENT_POINTER_MOVE && state->dragging) {
        state->pan_x += event->x - state->drag_x;
        state->pan_y += event->y - state->drag_y;
        state->drag_x = event->x;
        state->drag_y = event->y;
        int width = 0, height = 0;
        gui_app_client_size(context, &width, &height);
        gui_app_invalidate(context, 0, 24, width, height - 20);
        return GUI_APP_RESULT_HANDLED;
    }
    if (event->type == GUI_APP_EVENT_POINTER_UP && state->dragging) {
        state->dragging = 0;
        gui_app_release_pointer(context);
        return GUI_APP_RESULT_HANDLED;
    }
    if (event->type == GUI_APP_EVENT_KEY) {
        if (event->key == KEY_TAB) {
            int current = gui_app_focused_control(context);
            int next = gui_widget_focus_next(
                current >= VIEWER_CONTROL_OPEN && current <= VIEWER_CONTROL_ZOOM_IN
                    ? current - VIEWER_CONTROL_OPEN : -1,
                VIEWER_CONTROL_ZOOM_IN, (event->modifiers & SYS_INPUT_KEY_SHIFT) != 0, 0);
            gui_app_focus_control(context, next + VIEWER_CONTROL_OPEN);
            return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
        }
        if ((event->key == KEY_ENTER || event->key == KEY_SPACE) &&
            gui_app_focused_control(context) >= VIEWER_CONTROL_OPEN &&
            gui_app_focused_control(context) <= VIEWER_CONTROL_ZOOM_IN) {
            int control = gui_app_focused_control(context);
            if (control == VIEWER_CONTROL_OPEN) {
                (void)gui_app_open_file_picker(context, GUI_FILE_REQUEST_OPEN,
                    GUI_FILE_FILTER_BMP, state->path[0] ? state->path : "/");
                return GUI_APP_RESULT_HANDLED;
            }
            if (control == VIEWER_CONTROL_FIT) state->zoom_percent = 0;
            else if (control == VIEWER_CONTROL_100) state->zoom_percent = 100;
            else if (control == VIEWER_CONTROL_ZOOM_OUT) {
                if (!state->zoom_percent) state->zoom_percent = 100;
                if (state->zoom_percent > 25) state->zoom_percent -= 25;
            } else {
                if (!state->zoom_percent) state->zoom_percent = 100;
                if (state->zoom_percent < 400) state->zoom_percent += 25;
            }
            state->cache_w = state->cache_h = 0;
            return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
        }
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

static const gui_app_descriptor_t VIEWER_DESCRIPTOR = {
    "Viewer", sizeof(viewer_state_t), 560, 400, 280, 180, 0,
    viewer_open, viewer_close, viewer_draw, viewer_event, GUI_APP_VIEWER, 0,
    "viewer", "Viewer", 3, 1
};

const gui_app_descriptor_t* gui_viewer_app_descriptor(void) {
    return &VIEWER_DESCRIPTOR;
}
