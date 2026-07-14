#include "files_app.h"
#include "app_services.h"
#include "canvas.h"
#include "dirent.h"
#include "keyboard.h"
#include "user_lib.h"
#include "widgets.h"

#define TITLE_H 18
#define ROW_H 12
#define MAX_ROWS 256
#define FILES_DIALOG_NONE 0
#define FILES_DIALOG_NEW 1
#define FILES_DIALOG_RENAME 2
#define FILES_DIALOG_DELETE 3
#define COL_WIN_BG 0x00FFFFFFu
#define COL_FRAME 0x00000000u
#define COL_TEXT 0x00000000u
#define COL_SUBTEXT 0x00404040u
#define COL_HILIGHT 0x000060A0u
#define COL_HILIGHT_T 0x00FFFFFFu

typedef struct files_state {
    int scroll;
    int scroll_drag_offset;
    int selected;
    int hovered;
    int dialog;
    gui_text_input_t dialog_input;
    char cwd[256];
    char rows[MAX_ROWS][NAME_MAX + 1];
    int row_dir[MAX_ROWS];
    int row_count;
    char status[80];
} files_state_t;

static gui_window_t* g_last_window;
static int g_last_row = -1;
static uint32_t g_last_tick;

static void copy_text(char* dst, const char* src, unsigned int cap) {
    unsigned int i = 0;
    while (i + 1u < cap && src[i]) { dst[i] = src[i]; i++; }
    if (cap) dst[i] = 0;
}

static int same_text(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void append_path(char* path, const char* name, unsigned int cap) {
    unsigned int n = 0, i = 0;
    while (n < cap && path[n]) n++;
    if (n > 1u && n + 1u < cap) path[n++] = '/';
    while (n + 1u < cap && name[i]) path[n++] = name[i++];
    if (cap) path[n < cap ? n : cap - 1u] = 0;
}

static void parent_path(char* path) {
    unsigned int n = 0;
    while (path[n]) n++;
    while (n > 1u && path[n - 1u] == '/') n--;
    while (n > 1u && path[n - 1u] != '/') n--;
    if (n > 1u) n--;
    path[n ? n : 1u] = 0;
    if (!path[0]) { path[0] = '/'; path[1] = 0; }
}

static int name_compare(const char* a, const char* b) {
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    return *a ? 1 : (*b ? -1 : 0);
}

static files_state_t* state_of(gui_app_context_t* context) {
    return (files_state_t*)gui_app_state(context);
}

static void load_directory(files_state_t* state) {
    char selected[NAME_MAX + 1];
    int retain = state->selected >= 0 && state->selected < state->row_count;
    if (retain) copy_text(selected, state->rows[state->selected], sizeof(selected));
    state->row_count = 0;
    state->scroll = 0;
    if (!same_text(state->cwd, "/")) {
        copy_text(state->rows[state->row_count], "..", NAME_MAX + 1);
        state->row_dir[state->row_count++] = 1;
    }
    DIR* directory = opendir(state->cwd);
    if (!directory) {
        copy_text(state->rows[state->row_count], "<cannot open>", NAME_MAX + 1);
        state->row_dir[state->row_count++] = 0;
        return;
    }
    struct dirent* entry;
    while ((entry = readdir(directory)) && state->row_count < MAX_ROWS) {
        copy_text(state->rows[state->row_count], entry->d_name, NAME_MAX + 1);
        state->row_dir[state->row_count++] = entry->d_is_dir;
    }
    closedir(directory);
    int first = !same_text(state->cwd, "/") ? 1 : 0;
    for (int i = first; i + 1 < state->row_count; i++) {
        for (int j = i + 1; j < state->row_count; j++) {
            int swap = state->row_dir[i] < state->row_dir[j] ||
                (state->row_dir[i] == state->row_dir[j] &&
                 name_compare(state->rows[i], state->rows[j]) > 0);
            if (swap) {
                char name[NAME_MAX + 1]; int is_dir = state->row_dir[i];
                copy_text(name, state->rows[i], sizeof(name));
                copy_text(state->rows[i], state->rows[j], NAME_MAX + 1);
                copy_text(state->rows[j], name, NAME_MAX + 1);
                state->row_dir[i] = state->row_dir[j];
                state->row_dir[j] = is_dir;
            }
        }
    }
    state->selected = state->row_count ? 0 : -1;
    if (retain) for (int i = 0; i < state->row_count; i++) {
        if (same_text(state->rows[i], selected)) { state->selected = i; break; }
    }
}

static int selected_path(files_state_t* state, char* path, unsigned int cap) {
    if (state->selected < 0 || state->selected >= state->row_count ||
        same_text(state->rows[state->selected], "..")) return 0;
    copy_text(path, state->cwd, cap);
    append_path(path, state->rows[state->selected], cap);
    return 1;
}

static void activate(gui_app_context_t* context, int row, uint32_t ticks,
                     int force) {
    gui_window_t* window = gui_app_window(context);
    files_state_t* state = state_of(context);
    if (row < 0 || row >= state->row_count) return;
    state->selected = row;
    if (state->row_dir[row]) {
        if (!force) return;
        if (same_text(state->rows[row], "..")) parent_path(state->cwd);
        else append_path(state->cwd, state->rows[row], sizeof(state->cwd));
        load_directory(state);
        return;
    }
    int is_double = force || (g_last_window == window && g_last_row == row &&
                              (uint32_t)(ticks - g_last_tick) < 35u);
    if (is_double) {
        char target[256];
        copy_text(target, state->cwd, sizeof(target));
        append_path(target, state->rows[row], sizeof(target));
        int result = gui_app_services_open_path(context, target);
        copy_text(state->status, result == 1 ? "Opened in Editor" :
                  result == 2 ? "Opened in Viewer" :
                  result == 3 ? "Launching..." :
                  "No launcher for this file type", sizeof(state->status));
        g_last_window = 0; g_last_row = -1; g_last_tick = 0;
    } else {
        copy_text(state->status, "Double-click or Enter to open", sizeof(state->status));
        g_last_window = window; g_last_row = row; g_last_tick = ticks;
    }
}

static void confirm_dialog(files_state_t* state) {
    char source[256], target[256]; int rc = -1;
    if (state->dialog == FILES_DIALOG_NEW && state->dialog_input.text[0]) {
        copy_text(target, state->cwd, sizeof(target));
        append_path(target, state->dialog_input.text, sizeof(target));
        rc = sys_mkdir(target, 0755u);
        copy_text(state->status, rc == 0 ? "Folder created" :
                  "Could not create folder", sizeof(state->status));
    } else if (state->dialog == FILES_DIALOG_RENAME &&
               state->dialog_input.text[0] &&
               selected_path(state, source, sizeof(source))) {
        copy_text(target, state->cwd, sizeof(target));
        append_path(target, state->dialog_input.text, sizeof(target));
        rc = sys_rename(source, target);
        copy_text(state->status, rc == 0 ? "Item renamed" :
                  "Could not rename item", sizeof(state->status));
    } else if (state->dialog == FILES_DIALOG_DELETE &&
               selected_path(state, source, sizeof(source))) {
        rc = state->row_dir[state->selected] ? sys_rmdir(source) : sys_unlink(source);
        copy_text(state->status, rc == 0 ? "Item deleted" :
                  (state->row_dir[state->selected] ? "Directory is not empty" :
                   "Could not delete item"), sizeof(state->status));
    }
    state->dialog = FILES_DIALOG_NONE;
    if (rc == 0) load_directory(state);
}

static void properties(files_state_t* state) {
    char path[256]; sys_stat_info_t info;
    if (selected_path(state, path, sizeof(path)) && sys_stat_full(path, &info) == 0) {
        if (state->row_dir[state->selected]) copy_text(state->status, "Directory", sizeof(state->status));
        else {
            char reverse[16], number[16]; int n = 0;
            unsigned int size = info.size;
            if (!size) reverse[n++] = '0';
            while (size) { reverse[n++] = (char)('0' + size % 10u); size /= 10u; }
            for (int i = 0; i < n; i++) number[i] = reverse[n - i - 1];
            number[n] = 0;
            copy_text(state->status, "File, size ", sizeof(state->status));
            unsigned int at = 0; while (state->status[at]) at++;
            unsigned int i = 0; while (at + 1u < sizeof(state->status) && number[i]) state->status[at++] = number[i++];
            const char* suffix = " bytes"; i = 0;
            while (at + 1u < sizeof(state->status) && suffix[i]) state->status[at++] = suffix[i++];
            state->status[at] = 0;
        }
    }
}

static int edit_dialog(gui_text_input_t* input, const gui_app_event_t* event) {
    if (event->key == KEY_LEFT) return gui_text_input_command(input, GUI_TEXT_INPUT_LEFT);
    if (event->key == KEY_RIGHT) return gui_text_input_command(input, GUI_TEXT_INPUT_RIGHT);
    if (event->key == KEY_HOME) return gui_text_input_command(input, GUI_TEXT_INPUT_HOME);
    if (event->key == KEY_END) return gui_text_input_command(input, GUI_TEXT_INPUT_END);
    if (event->key == KEY_BACKSPACE) return gui_text_input_command(input, GUI_TEXT_INPUT_BACKSPACE);
    if (event->key == KEY_DELETE) return gui_text_input_command(input, GUI_TEXT_INPUT_DELETE);
    return (event->ascii & 0xffu) >= 32u ?
        gui_text_input_insert(input, (char)(event->ascii & 0xffu)) : 0;
}

static void open_files_dialog(gui_app_context_t* context, int kind,
                              const char* initial_text) {
    static const gui_dialog_button_t edit_buttons[] = {
        {1u, "OK", 1, 0}, {2u, "Cancel", 0, 1}
    };
    static const gui_dialog_button_t delete_buttons[] = {
        {1u, "Delete", 1, 0}, {2u, "Cancel", 0, 1}
    };
    gui_dialog_request_t request;
    files_state_t* state = state_of(context);
    request.request_id = (unsigned int)kind;
    request.title = kind == FILES_DIALOG_NEW ? "New Folder" :
                    kind == FILES_DIALOG_RENAME ? "Rename" : "Delete";
    request.message = kind == FILES_DIALOG_DELETE
        ? "Delete selected item?" : "Name:";
    request.initial_text = kind == FILES_DIALOG_DELETE ? 0 : initial_text;
    request.buttons = kind == FILES_DIALOG_DELETE
        ? delete_buttons : edit_buttons;
    request.button_count = 2;
    if (gui_app_open_dialog(context, &request)) state->dialog = kind;
    else copy_text(state->status, "Another dialog is already open",
                   sizeof(state->status));
}

static void files_open(gui_app_context_t* context, const char* argument) {
    files_state_t* state = state_of(context); (void)argument;
    copy_text(state->cwd, "/", sizeof(state->cwd));
    copy_text(state->status, "Double-click files to open", sizeof(state->status));
    state->selected = -1;
    state->hovered = -1;
    load_directory(state);
}

static void files_draw(gfx_surface_t* surface, gui_app_context_t* context,
                       int mouse_x, int mouse_y) {
    files_state_t* state = state_of(context);
    int bx = 0, by = 0, bw = (int)surface->width;
    int bh = (int)surface->height, row_top = 36, status_h = 13;
    int row_area = bh - 36 - status_h, visible = row_area / ROW_H;
    const gui_widget_theme_t* theme = gui_app_services_widget_theme();
    int focus = gui_app_focused_control(context);
    int captured = gui_app_captured_control(context);
    if (visible < 1) visible = 1;
    gui_canvas_fill_rect(surface, bx, by, bw, bh, COL_WIN_BG);
    gui_app_services_draw_text(surface, bx + 4, by + 2, "Path:", COL_SUBTEXT);
    gui_app_services_draw_text(surface, bx + 36, by + 2, state->cwd, COL_TEXT);
    gui_canvas_hline(surface, bx, by + 12, bw, COL_FRAME);
    gui_widget_button(surface, gui_rect_make(bx + 4, by + 15, 42, 18), "New", (gui_widget_state_t){0,captured == 2,focus == 2,0}, theme, gui_app_services_draw_text);
    gui_widget_button(surface, gui_rect_make(bx + 50, by + 15, 54, 18), "Rename", (gui_widget_state_t){0,captured == 3,focus == 3,state->selected < 0 || same_text(state->rows[state->selected], "..")}, theme, gui_app_services_draw_text);
    gui_widget_button(surface, gui_rect_make(bx + 108, by + 15, 48, 18), "Delete", (gui_widget_state_t){0,captured == 4,focus == 4,state->selected < 0 || same_text(state->rows[state->selected], "..")}, theme, gui_app_services_draw_text);
    gui_widget_button(surface, gui_rect_make(bx + 160, by + 15, 62, 18), "Properties", (gui_widget_state_t){0,captured == 5,focus == 5,state->selected < 0}, theme, gui_app_services_draw_text);
    for (int i = 0; i < visible && i + state->scroll < state->row_count; i++) {
        int row = i + state->scroll, y = row_top + i * ROW_H;
        int hover = row == state->hovered;
        int selected = row == state->selected;
        if (hover || selected) gui_canvas_fill_rect(surface, bx, y, bw - 12, ROW_H, COL_HILIGHT);
        unsigned int color = hover || selected ? COL_HILIGHT_T : COL_TEXT;
        gui_app_services_draw_text(surface, bx + 4, y + 2, state->row_dir[row] ? "[D]" : "[F]", color);
        gui_app_services_draw_text(surface, bx + 28, y + 2, state->rows[row], color);
    }
    gui_widget_scrollbar(surface, gui_rect_make(bw - 10, 36, 10, row_area), state->row_count, visible, state->scroll, (gui_widget_state_t){0,gui_app_captured_control(context) == 1,0,0}, theme);
    gui_canvas_hline(surface, bx, by + bh - status_h, bw, COL_FRAME);
    if (state->status[0]) gui_app_services_draw_text(surface, bx + 4, by + bh - status_h + 4, state->status, COL_SUBTEXT);
}

static unsigned int files_event(gui_app_context_t* context,
                                const gui_app_event_t* event) {
    files_state_t* state = state_of(context);
    int width = 0, height = 0;
    gui_app_client_size(context, &width, &height);
    int row_area = height - 49, visible = row_area / ROW_H;
    if (visible < 1) visible = 1;
    int max_scroll = state->row_count - visible; if (max_scroll < 0) max_scroll = 0;
    gui_rect_t track = gui_rect_make(width - 10, 36, 10, row_area);
    gui_rect_t thumb = gui_widget_scroll_thumb(track, state->row_count, visible, state->scroll);
    gui_rect_t command_bounds[] = {
        {0,0,0,0}, {0,0,0,0}, {4,15,42,18}, {50,15,54,18},
        {108,15,48,18}, {160,15,62,18}
    };
    if (event->type == GUI_APP_EVENT_DIALOG_RESULT &&
        event->request_id >= FILES_DIALOG_NEW &&
        event->request_id <= FILES_DIALOG_DELETE) {
        int kind = (int)event->request_id;
        state->dialog = kind;
        if (event->button_id == 1u) {
            if (event->text) gui_text_input_init(&state->dialog_input,
                                                 event->text);
            confirm_dialog(state);
        } else state->dialog = FILES_DIALOG_NONE;
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_POINTER_MOVE && gui_app_captured_control(context) != 1) {
        int hovered = -1;
        if (event->x >= 0 && event->x < width - 12 && event->y >= 36 &&
            event->y < 36 + visible * ROW_H) {
            hovered = (event->y - 36) / ROW_H + state->scroll;
            if (hovered >= state->row_count) hovered = -1;
        }
        if (hovered != state->hovered) {
            int old = state->hovered;
            state->hovered = hovered;
            if (old >= state->scroll)
                gui_app_invalidate(context, 0, 36 + (old - state->scroll) * ROW_H,
                                   width - 12, ROW_H);
            if (hovered >= state->scroll)
                gui_app_invalidate(context, 0, 36 + (hovered - state->scroll) * ROW_H,
                                   width - 12, ROW_H);
        }
        return GUI_APP_RESULT_HANDLED;
    }
    if (event->type == GUI_APP_EVENT_WHEEL) state->scroll -= event->wheel * 3;
    else if (event->type == GUI_APP_EVENT_KEY) {
        if (event->key == KEY_TAB) {
            unsigned char enabled[] = {1,
                state->selected >= 0 && !same_text(state->rows[state->selected], ".."),
                state->selected >= 0 && !same_text(state->rows[state->selected], ".."),
                state->selected >= 0};
            int current = gui_app_focused_control(context);
            int next = gui_widget_focus_next(
                current >= 2 && current <= 5 ? current - 2 : -1, 4,
                (event->modifiers & SYS_INPUT_KEY_SHIFT) != 0, enabled);
            gui_app_focus_control(context, next + 2);
        }
        else if ((event->key == KEY_ENTER || event->key == KEY_SPACE) &&
                 gui_app_focused_control(context) >= 2 &&
                 gui_app_focused_control(context) <= 5) {
            int control = gui_app_focused_control(context);
            if (control == 2) open_files_dialog(context, FILES_DIALOG_NEW, "New Folder");
            else if (control == 3 && state->selected >= 0 && !same_text(state->rows[state->selected],"..")) open_files_dialog(context, FILES_DIALOG_RENAME, state->rows[state->selected]);
            else if (control == 4 && state->selected >= 0 && !same_text(state->rows[state->selected],"..")) open_files_dialog(context, FILES_DIALOG_DELETE, 0);
            else if (control == 5 && state->selected >= 0) properties(state);
        }
        else if ((event->modifiers & SYS_INPUT_KEY_CTRL) && (event->ascii == 'n' || event->ascii == 'N')) open_files_dialog(context, FILES_DIALOG_NEW, "New Folder");
        else if (event->key == KEY_F2 && state->selected >= 0 && !same_text(state->rows[state->selected], "..")) open_files_dialog(context, FILES_DIALOG_RENAME, state->rows[state->selected]);
        else if (event->key == KEY_DELETE && state->selected >= 0 && !same_text(state->rows[state->selected], "..")) open_files_dialog(context, FILES_DIALOG_DELETE, 0);
        else if ((event->modifiers & SYS_INPUT_KEY_ALT) && event->key == KEY_ENTER) properties(state);
        else if (event->key == KEY_UP && state->selected > 0) state->selected--;
        else if (event->key == KEY_DOWN && state->selected + 1 < state->row_count) state->selected++;
        else if (event->key == KEY_HOME && state->row_count) state->selected = 0;
        else if (event->key == KEY_END && state->row_count) state->selected = state->row_count - 1;
        else if (event->key == KEY_PAGEUP && state->row_count) { state->selected -= visible; if (state->selected < 0) state->selected = 0; }
        else if (event->key == KEY_PAGEDOWN && state->row_count) { state->selected += visible; if (state->selected >= state->row_count) state->selected = state->row_count - 1; }
        else if (event->key == KEY_ENTER && state->selected >= 0) activate(context, state->selected, event->ticks, 1);
        else return GUI_APP_RESULT_NONE;
        if (state->selected < state->scroll) state->scroll = state->selected;
        if (state->selected >= state->scroll + visible)
            state->scroll = state->selected - visible + 1;
    } else if (event->type == GUI_APP_EVENT_POINTER_MOVE && gui_app_captured_control(context) == 1) {
        state->scroll = gui_widget_scroll_offset(track, state->row_count, visible, event->y, state->scroll_drag_offset);
    } else if (event->type == GUI_APP_EVENT_POINTER_UP && gui_app_captured_control(context) == 1) gui_app_release_pointer(context);
    else if (event->type == GUI_APP_EVENT_POINTER_UP &&
             gui_app_captured_control(context) >= 2) {
        int control = gui_app_captured_control(context);
        gui_app_release_pointer(context);
        if (control <= 5 && gui_widget_hit(command_bounds[control], event->x,event->y)) {
            if (control == 2) open_files_dialog(context, FILES_DIALOG_NEW, "New Folder");
            else if (control == 3 && state->selected >= 0 && !same_text(state->rows[state->selected],"..")) open_files_dialog(context, FILES_DIALOG_RENAME, state->rows[state->selected]);
            else if (control == 4 && state->selected >= 0 && !same_text(state->rows[state->selected],"..")) open_files_dialog(context, FILES_DIALOG_DELETE, 0);
            else if (control == 5 && state->selected >= 0) properties(state);
        }
    }
    else if (event->type == GUI_APP_EVENT_POINTER_DOWN) {
        int command = 0;
        for (int i = 2; i <= 5; i++)
            if (gui_widget_hit(command_bounds[i], event->x, event->y)) command = i;
        if ((command == 3 || command == 4) &&
            (state->selected < 0 || same_text(state->rows[state->selected], ".."))) command = 0;
        if (command == 5 && state->selected < 0) command = 0;
        if (command) {
            gui_app_focus_control(context, command);
            gui_app_capture_pointer(context, command);
        } else if (gui_widget_hit(track,event->x,event->y)) {
            if (gui_widget_hit(thumb,event->x,event->y)) { gui_app_capture_pointer(context, 1); state->scroll_drag_offset=event->y-thumb.y; }
            else state->scroll += event->y < thumb.y ? -visible : visible;
        } else if (event->y >= 36 && event->y < 36 + visible * ROW_H) {
            int row=(event->y-36)/ROW_H+state->scroll;
            if (row >= 0 && row < state->row_count) activate(context,row,event->ticks,state->row_dir[row]);
        }
    } else return GUI_APP_RESULT_NONE;
    if (state->scroll < 0) state->scroll = 0;
    if (state->scroll > max_scroll) state->scroll = max_scroll;
    return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
}

static const gui_app_descriptor_t DESCRIPTOR = {
    "Files", sizeof(files_state_t), 360, 240, 220, 120, 0,
    files_open, 0, files_draw, files_event, GUI_APP_FILES, 0,
    "files", "Files", 0, 1
};

const gui_app_descriptor_t* gui_files_app_descriptor(void) { return &DESCRIPTOR; }
