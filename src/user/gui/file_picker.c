#include "file_picker.h"

#include "dirent.h"
#include "keyboard.h"
#include "layout.h"
#include "smallos_input.h"

#define PICKER_ROW_H 12
#define PICKER_FOCUS_LIST 1
#define PICKER_FOCUS_FILENAME 2
#define PICKER_FOCUS_ACCEPT 3
#define PICKER_FOCUS_CANCEL 4
#define PICKER_PRESS_SCROLL 5
#define PICKER_DOUBLE_TICKS 120u

static unsigned int picker_strlen(const char* text) {
    unsigned int length = 0;
    while (text && text[length]) length++;
    return length;
}

static void picker_copy(char* out, const char* text, unsigned int capacity) {
    unsigned int i = 0;
    if (!out || capacity == 0u) return;
    while (text && text[i] && i + 1u < capacity) {
        out[i] = text[i];
        i++;
    }
    out[i] = '\0';
}

static void picker_append(char* out, const char* text,
                          unsigned int capacity) {
    unsigned int used = picker_strlen(out);
    while (text && *text && used + 1u < capacity)
        out[used++] = *text++;
    out[used] = '\0';
}

static void picker_parent(char* path) {
    unsigned int length = picker_strlen(path);
    int slash = -1;
    if (length > 1u && path[length - 1u] == '/') path[--length] = '\0';
    for (unsigned int i = 0; i < length; i++)
        if (path[i] == '/') slash = (int)i;
    if (slash <= 0) picker_copy(path, "/", GUI_FILE_PICKER_PATH_MAX);
    else path[slash] = '\0';
}

static void picker_join(char* out, const char* directory, const char* name) {
    picker_copy(out, directory && directory[0] ? directory : "/",
                GUI_FILE_PICKER_PATH_MAX);
    if (picker_strlen(out) > 1u && out[picker_strlen(out) - 1u] == '/')
        out[picker_strlen(out) - 1u] = '\0';
    if (out[0] != '/' || out[1] != '\0') picker_append(out, "/",
                                                       GUI_FILE_PICKER_PATH_MAX);
    picker_append(out, name, GUI_FILE_PICKER_PATH_MAX);
}

static void picker_split_path(const char* path, char* directory,
                              char* filename) {
    unsigned int length = picker_strlen(path);
    int slash = -1;
    for (unsigned int i = 0; i < length; i++)
        if (path[i] == '/') slash = (int)i;
    if (!path || !path[0]) {
        picker_copy(directory, "/", GUI_FILE_PICKER_PATH_MAX);
        filename[0] = '\0';
    } else if (slash < 0) {
        picker_copy(directory, "/", GUI_FILE_PICKER_PATH_MAX);
        picker_copy(filename, path, GUI_TEXT_INPUT_CAPACITY);
    } else {
        if (slash == 0) picker_copy(directory, "/", GUI_FILE_PICKER_PATH_MAX);
        else {
            unsigned int count = (unsigned int)slash;
            if (count >= GUI_FILE_PICKER_PATH_MAX)
                count = GUI_FILE_PICKER_PATH_MAX - 1u;
            for (unsigned int i = 0; i < count; i++) directory[i] = path[i];
            directory[count] = '\0';
        }
        picker_copy(filename, path + slash + 1, GUI_TEXT_INPUT_CAPACITY);
    }
}

static void picker_load(gui_file_picker_t* picker) {
    DIR* directory;
    struct dirent* entry;
    picker->row_count = 0;
    picker->selected = -1;
    picker->scroll = 0;
    if (!(picker->cwd[0] == '/' && picker->cwd[1] == '\0')) {
        picker_copy(picker->rows[picker->row_count], "..",
                    GUI_FILE_PICKER_PATH_MAX);
        picker->row_is_dir[picker->row_count++] = 1u;
    }
    directory = opendir(picker->cwd);
    if (!directory) return;
    while ((entry = readdir(directory)) != 0 &&
           picker->row_count < GUI_FILE_PICKER_MAX_ROWS) {
        picker_copy(picker->rows[picker->row_count], entry->d_name,
                    GUI_FILE_PICKER_PATH_MAX);
        picker->row_is_dir[picker->row_count] = entry->d_is_dir ? 1u : 0u;
        picker->row_count++;
    }
    closedir(directory);
}

void gui_file_picker_init(gui_file_picker_t* picker,
                          gui_file_picker_mode_t mode,
                          const char* initial_path) {
    char filename[GUI_TEXT_INPUT_CAPACITY];
    if (!picker) return;
    picker->mode = mode;
    picker_split_path(initial_path, picker->cwd, filename);
    picker->focused = PICKER_FOCUS_LIST;
    picker->pressed = 0;
    picker->last_click_row = -1;
    picker->last_click_ticks = 0;
    picker->result_path[0] = '\0';
    gui_text_input_init(&picker->filename, filename);
    picker_load(picker);
}

void gui_file_picker_layout(gui_rect_t bounds,
                            gui_file_picker_layout_t* out) {
    gui_rect_t inner;
    gui_rect_t buttons;
    gui_vlayout_t vertical;
    if (!out) return;
    inner = gui_layout_inset(bounds, 8);
    gui_vlayout_begin(&vertical, inner, 4);
    out->path = gui_vlayout_take(&vertical, 10);
    out->list = gui_vlayout_take(&vertical, inner.h - 66);
    out->filename = gui_vlayout_take(&vertical, 18);
    buttons = gui_vlayout_take(&vertical, 20);
    out->accept = gui_layout_cell(buttons, 2, 8, 0);
    out->cancel = gui_layout_cell(buttons, 2, 8, 1);
    out->scrollbar = gui_rect_make(out->list.x + out->list.w - 10,
                                   out->list.y, 10, out->list.h);
}

static int picker_visible_rows(const gui_file_picker_layout_t* layout) {
    int visible = layout->list.h / PICKER_ROW_H;
    return visible < 1 ? 1 : visible;
}

static void picker_clamp_scroll(gui_file_picker_t* picker, int visible) {
    int maximum = picker->row_count - visible;
    if (maximum < 0) maximum = 0;
    if (picker->scroll < 0) picker->scroll = 0;
    if (picker->scroll > maximum) picker->scroll = maximum;
    if (picker->selected >= 0) {
        if (picker->selected < picker->scroll)
            picker->scroll = picker->selected;
        if (picker->selected >= picker->scroll + visible)
            picker->scroll = picker->selected - visible + 1;
    }
}

static void picker_short_text(char* out, const char* text, int pixels) {
    int capacity = pixels / 6;
    int i = 0;
    if (capacity < 1) { out[0] = '\0'; return; }
    while (text && text[i] && i < capacity &&
           i + 1 < GUI_FILE_PICKER_PATH_MAX) {
        out[i] = text[i];
        i++;
    }
    out[i] = '\0';
}

void gui_file_picker_draw(gfx_surface_t* surface,
                          gui_file_picker_t* picker,
                          gui_rect_t bounds,
                          const gui_widget_theme_t* theme,
                          const gui_file_picker_style_t* style,
                          gui_widget_text_fn draw_text) {
    gui_file_picker_layout_t layout;
    int visible;
    char text[GUI_FILE_PICKER_PATH_MAX];
    char heading[GUI_FILE_PICKER_PATH_MAX];
    if (!surface || !picker || !theme || !style) return;
    gui_file_picker_layout(bounds, &layout);
    visible = picker_visible_rows(&layout);
    picker_clamp_scroll(picker, visible);
    gui_canvas_fill_rect(surface, bounds.x, bounds.y, bounds.w, bounds.h,
                         style->panel);
    gui_canvas_rect(surface, bounds.x, bounds.y, bounds.w, bounds.h,
                    style->frame);
    picker_copy(heading,
        picker->mode == GUI_FILE_PICKER_SAVE ? "Save As: " : "Open: ",
        sizeof(heading));
    picker_append(heading, picker->cwd, sizeof(heading));
    picker_short_text(text, heading, layout.path.w);
    if (draw_text) draw_text(surface, layout.path.x, layout.path.y,
                             text, style->subtext);
    gui_canvas_fill_rect(surface, layout.list.x, layout.list.y,
                         layout.list.w, layout.list.h, style->panel);
    gui_canvas_rect(surface, layout.list.x, layout.list.y,
                    layout.list.w, layout.list.h, style->frame);
    for (int i = 0; i < visible; i++) {
        int row = picker->scroll + i;
        int y = layout.list.y + i * PICKER_ROW_H;
        if (row >= picker->row_count) break;
        if (row == picker->selected)
            gui_canvas_fill_rect(surface, layout.list.x + 1, y + 1,
                                 layout.list.w - 12, PICKER_ROW_H - 1,
                                 style->selection);
        picker_short_text(text, picker->rows[row], layout.list.w - 30);
        if (draw_text) {
            draw_text(surface, layout.list.x + 3, y + 3,
                      picker->row_is_dir[row] ? "[D]" : "[F]",
                      row == picker->selected ? style->selected_text :
                                                style->subtext);
            draw_text(surface, layout.list.x + 27, y + 3, text,
                      row == picker->selected ? style->selected_text :
                                                style->text);
        }
    }
    gui_widget_scrollbar(surface, layout.scrollbar, picker->row_count,
        visible, picker->scroll,
        (gui_widget_state_t){0, picker->pressed == PICKER_PRESS_SCROLL,
                             picker->focused == PICKER_FOCUS_LIST, 0}, theme);
    gui_widget_text_field(surface, layout.filename, picker->filename.text,
        picker->filename.cursor,
        (gui_widget_state_t){0, 0,
            picker->focused == PICKER_FOCUS_FILENAME, 0}, theme, draw_text);
    gui_widget_button(surface, layout.accept,
        picker->mode == GUI_FILE_PICKER_SAVE ? "Save" : "Open",
        (gui_widget_state_t){0, picker->pressed == PICKER_FOCUS_ACCEPT,
            picker->focused == PICKER_FOCUS_ACCEPT,
            picker->filename.length == 0}, theme, draw_text);
    gui_widget_button(surface, layout.cancel, "Cancel",
        (gui_widget_state_t){0, picker->pressed == PICKER_FOCUS_CANCEL,
            picker->focused == PICKER_FOCUS_CANCEL, 0}, theme, draw_text);
}

static void picker_choose_row(gui_file_picker_t* picker, int row) {
    picker->selected = row;
    picker->focused = PICKER_FOCUS_LIST;
    if (row >= 0 && row < picker->row_count && !picker->row_is_dir[row])
        gui_text_input_init(&picker->filename, picker->rows[row]);
}

static int picker_accept(gui_file_picker_t* picker) {
    if (picker->filename.length == 0) return 0;
    picker_join(picker->result_path, picker->cwd, picker->filename.text);
    return 1;
}

static int picker_open_selected(gui_file_picker_t* picker) {
    int row = picker->selected;
    if (row < 0 || row >= picker->row_count) return 0;
    if (!picker->row_is_dir[row]) return picker_accept(picker);
    if (picker->rows[row][0] == '.' && picker->rows[row][1] == '.' &&
        picker->rows[row][2] == '\0')
        picker_parent(picker->cwd);
    else {
        char path[GUI_FILE_PICKER_PATH_MAX];
        picker_join(path, picker->cwd, picker->rows[row]);
        picker_copy(picker->cwd, path, sizeof(picker->cwd));
    }
    picker_load(picker);
    return 0;
}

static gui_file_picker_result_t picker_key(gui_file_picker_t* picker,
                                            const gui_app_event_t* event,
                                            int visible) {
    if (event->key == KEY_ESC) return GUI_FILE_PICKER_RESULT_CANCEL;
    if (event->key == KEY_TAB) {
        int backwards = (event->modifiers & SYS_INPUT_KEY_SHIFT) != 0u;
        picker->focused += backwards ? -1 : 1;
        if (picker->focused < PICKER_FOCUS_LIST)
            picker->focused = PICKER_FOCUS_CANCEL;
        if (picker->focused > PICKER_FOCUS_CANCEL)
            picker->focused = PICKER_FOCUS_LIST;
        return GUI_FILE_PICKER_RESULT_REDRAW;
    }
    if (picker->focused == PICKER_FOCUS_LIST) {
        if (event->key == KEY_UP) {
            if (picker->selected > 0) picker_choose_row(picker,
                                                        picker->selected - 1);
            else if (picker->selected < 0 && picker->row_count)
                picker_choose_row(picker, 0);
        } else if (event->key == KEY_DOWN) {
            if (picker->selected + 1 < picker->row_count)
                picker_choose_row(picker, picker->selected + 1);
        } else if (event->key == KEY_PAGEUP) {
            picker->selected -= visible;
            if (picker->selected < 0) picker->selected = 0;
        } else if (event->key == KEY_PAGEDOWN) {
            picker->selected += visible;
            if (picker->selected >= picker->row_count)
                picker->selected = picker->row_count - 1;
        } else if (event->key == KEY_ENTER) {
            if (picker_open_selected(picker))
                return GUI_FILE_PICKER_RESULT_ACCEPT;
        } else {
            return GUI_FILE_PICKER_RESULT_NONE;
        }
        picker_clamp_scroll(picker, visible);
        return GUI_FILE_PICKER_RESULT_REDRAW;
    }
    if (picker->focused == PICKER_FOCUS_FILENAME) {
        gui_text_input_command_t command = 0;
        if (event->key == KEY_LEFT) command = GUI_TEXT_INPUT_LEFT;
        else if (event->key == KEY_RIGHT) command = GUI_TEXT_INPUT_RIGHT;
        else if (event->key == KEY_HOME) command = GUI_TEXT_INPUT_HOME;
        else if (event->key == KEY_END) command = GUI_TEXT_INPUT_END;
        else if (event->key == KEY_BACKSPACE) command = GUI_TEXT_INPUT_BACKSPACE;
        else if (event->key == KEY_DELETE) command = GUI_TEXT_INPUT_DELETE;
        else if (event->key == KEY_ENTER)
            return picker_accept(picker) ? GUI_FILE_PICKER_RESULT_ACCEPT :
                                           GUI_FILE_PICKER_RESULT_REDRAW;
        else if ((event->ascii & 0xFFu) >= 32u)
            (void)gui_text_input_insert(&picker->filename,
                                        (char)(event->ascii & 0xFFu));
        else return GUI_FILE_PICKER_RESULT_NONE;
        if (command) (void)gui_text_input_command(&picker->filename, command);
        return GUI_FILE_PICKER_RESULT_REDRAW;
    }
    if (event->key == KEY_ENTER) {
        if (picker->focused == PICKER_FOCUS_CANCEL)
            return GUI_FILE_PICKER_RESULT_CANCEL;
        return picker_accept(picker) ? GUI_FILE_PICKER_RESULT_ACCEPT :
                                       GUI_FILE_PICKER_RESULT_REDRAW;
    }
    return GUI_FILE_PICKER_RESULT_NONE;
}

gui_file_picker_result_t gui_file_picker_event(
    gui_file_picker_t* picker, const gui_app_event_t* event,
    gui_rect_t bounds) {
    gui_file_picker_layout_t layout;
    gui_rect_t thumb;
    int visible;
    if (!picker || !event) return GUI_FILE_PICKER_RESULT_NONE;
    gui_file_picker_layout(bounds, &layout);
    visible = picker_visible_rows(&layout);
    thumb = gui_widget_scroll_thumb(layout.scrollbar, picker->row_count,
                                    visible, picker->scroll);
    if (event->type == GUI_APP_EVENT_KEY)
        return picker_key(picker, event, visible);
    if (event->type == GUI_APP_EVENT_WHEEL) {
        picker->scroll -= event->wheel * 3;
        picker_clamp_scroll(picker, visible);
        return GUI_FILE_PICKER_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_POINTER_MOVE &&
        picker->pressed == PICKER_PRESS_SCROLL) {
        picker->scroll = gui_widget_scroll_offset(layout.scrollbar,
            picker->row_count, visible, event->y,
            picker->scroll_drag_offset);
        return GUI_FILE_PICKER_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_POINTER_UP) {
        int pressed = picker->pressed;
        picker->pressed = 0;
        if (pressed == PICKER_FOCUS_ACCEPT &&
            gui_widget_hit(layout.accept, event->x, event->y))
            return picker_accept(picker) ? GUI_FILE_PICKER_RESULT_ACCEPT :
                                           GUI_FILE_PICKER_RESULT_REDRAW;
        if (pressed == PICKER_FOCUS_CANCEL &&
            gui_widget_hit(layout.cancel, event->x, event->y))
            return GUI_FILE_PICKER_RESULT_CANCEL;
        return GUI_FILE_PICKER_RESULT_REDRAW;
    }
    if (event->type != GUI_APP_EVENT_POINTER_DOWN)
        return GUI_FILE_PICKER_RESULT_NONE;
    if (gui_widget_hit(layout.scrollbar, event->x, event->y)) {
        if (gui_widget_hit(thumb, event->x, event->y)) {
            picker->pressed = PICKER_PRESS_SCROLL;
            picker->scroll_drag_offset = event->y - thumb.y;
        } else {
            picker->scroll += event->y < thumb.y ? -visible : visible;
            picker_clamp_scroll(picker, visible);
        }
        return GUI_FILE_PICKER_RESULT_REDRAW;
    }
    if (gui_widget_hit(layout.list, event->x, event->y)) {
        int row = picker->scroll + (event->y - layout.list.y) / PICKER_ROW_H;
        if (row >= 0 && row < picker->row_count) {
            int double_click = row == picker->last_click_row &&
                event->ticks - picker->last_click_ticks <= PICKER_DOUBLE_TICKS;
            picker_choose_row(picker, row);
            picker->last_click_row = row;
            picker->last_click_ticks = event->ticks;
            if (double_click && picker_open_selected(picker))
                return GUI_FILE_PICKER_RESULT_ACCEPT;
        }
        return GUI_FILE_PICKER_RESULT_REDRAW;
    }
    if (gui_widget_hit(layout.filename, event->x, event->y)) {
        picker->focused = PICKER_FOCUS_FILENAME;
        picker->filename.cursor = (event->x - layout.filename.x - 3) / 6;
        if (picker->filename.cursor < 0) picker->filename.cursor = 0;
        if (picker->filename.cursor > picker->filename.length)
            picker->filename.cursor = picker->filename.length;
        return GUI_FILE_PICKER_RESULT_REDRAW;
    }
    if (gui_widget_hit(layout.accept, event->x, event->y)) {
        picker->focused = PICKER_FOCUS_ACCEPT;
        picker->pressed = PICKER_FOCUS_ACCEPT;
        return GUI_FILE_PICKER_RESULT_REDRAW;
    }
    if (gui_widget_hit(layout.cancel, event->x, event->y)) {
        picker->focused = PICKER_FOCUS_CANCEL;
        picker->pressed = PICKER_FOCUS_CANCEL;
        return GUI_FILE_PICKER_RESULT_REDRAW;
    }
    return GUI_FILE_PICKER_RESULT_NONE;
}

const char* gui_file_picker_path(const gui_file_picker_t* picker) {
    return picker ? picker->result_path : "";
}
