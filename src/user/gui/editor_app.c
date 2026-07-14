#include "editor_app.h"
#include "app_services.h"
#include "canvas.h"
#include "file_picker.h"
#include "framework_internal.h"
#include "keyboard.h"
#include "user_lib.h"
#include "widgets.h"
#include "../editor_model.h"

#define TITLE_H 18
#define COL_WIN_BG 0x00FFFFFFu
#define COL_FRAME 0x00000000u
#define COL_TEXT 0x00000000u
#define COL_SUBTEXT 0x00404040u
#define COL_HILIGHT 0x000060A0u
#define COL_HILIGHT_T 0x00FFFFFFu
#define COL_TITLE_BG 0x00000000u
#define GUI_WIDGET_THEME (*gui_app_services_widget_theme())
#define draw_text gui_app_services_draw_text
#define make_rect gui_rect_make
#define fillr gui_canvas_fill_rect
#define hline gui_canvas_hline
#define vline gui_canvas_vline
#define rect gui_canvas_rect
#define GUI_CLIPBOARD_CAPACITY 8192u

typedef gui_window_t window_t;

typedef struct {
    editor_model_t model;
    uint32_t row;
    uint32_t column;
    uint32_t top;
    uint32_t hscroll;
    uint32_t next_blink;
    int caret_visible;
    int confirm_close;
    int confirm_choice;
    uint32_t anchor_row;
    uint32_t anchor_column;
    int selection_active;
    int scroll_drag_offset;
    int pending_action;
    int picker_active;
    int picker_after_save_action;
    gui_file_picker_t picker;
    char status[80];
} editor_window_state_t;

#define EDITOR_STATE(w) ((editor_window_state_t*)(w)->state)
static char g_editor_clipboard[GUI_CLIPBOARD_CAPACITY];

static unsigned int u_strlen(const char* text) {
    unsigned int length = 0; while (text[length]) length++; return length;
}
static void u_strcpy_n(char* dst, const char* src, unsigned int capacity) {
    unsigned int i = 0;
    while (i + 1u < capacity && src[i]) { dst[i] = src[i]; i++; }
    if (capacity) dst[i] = 0;
}
static void u_strcat_n(char* dst, const char* src, unsigned int capacity) {
    unsigned int n = u_strlen(dst), i = 0;
    while (n + 1u < capacity && src[i]) dst[n++] = src[i++];
    if (capacity) dst[n] = 0;
}

static void utoa10(unsigned int value, char* out) {
    char reverse[16]; int count = 0;
    if (!value) { out[0] = '0'; out[1] = 0; return; }
    while (value && count < 16) {
        reverse[count++] = (char)('0' + value % 10u); value /= 10u;
    }
    for (int i = 0; i < count; i++) out[i] = reverse[count - i - 1];
    out[count] = 0;
}

static void draw_char(gfx_surface_t* surface, int x, int y, char ch,
                      unsigned int color) {
    char text[2] = {ch, 0};
    draw_text(surface, x, y, text, color);
}

static void draw_fixed_text(gfx_surface_t* surface, int x, int y,
                            const char* text, int max_chars,
                            unsigned int color) {
    char clipped[GUI_FILE_PICKER_PATH_MAX];
    int count = 0;
    while (count < max_chars && count + 1 < (int)sizeof(clipped) && text[count]) {
        clipped[count] = text[count]; count++;
    }
    clipped[count] = 0;
    draw_text(surface, x, y, clipped, color);
}

#define EDITOR_TOOLBAR_H 24
#define EDITOR_STATUS_H 14
#define EDITOR_LINE_H 8
#define EDITOR_WIDGET_SAVE 1
#define EDITOR_WIDGET_DISCARD 2
#define EDITOR_WIDGET_CANCEL 3
#define EDITOR_WIDGET_SCROLLBAR 10
#define EDITOR_WIDGET_TEXT 11
#define EDITOR_WIDGET_NEW 20
#define EDITOR_WIDGET_OPEN 21
#define EDITOR_WIDGET_SAVE_AS 22
#define EDITOR_WIDGET_PICKER 30
#define EDITOR_PENDING_NONE 0
#define EDITOR_PENDING_CLOSE 1
#define EDITOR_PENDING_NEW 2
#define EDITOR_PENDING_OPEN 3

static const gui_file_picker_style_t EDITOR_PICKER_STYLE = {
    COL_WIN_BG, COL_FRAME, COL_TEXT, COL_SUBTEXT,
    COL_HILIGHT, COL_HILIGHT_T
};

static void editor_set_status(editor_window_state_t* editor,
                              const char* status) {
    u_strcpy_n(editor->status, status, sizeof(editor->status));
}

static gui_rect_t editor_toolbar_button(const window_t* w, int widget,
                                        int absolute) {
    int x = widget == EDITOR_WIDGET_NEW ? 4 :
            widget == EDITOR_WIDGET_OPEN ? 48 : 92;
    int width = widget == EDITOR_WIDGET_SAVE_AS ? 60 : 40;
    return make_rect(x + (absolute ? w->x : 0),
                     3 + (absolute ? w->y + TITLE_H : 0), width, 18);
}

static gui_rect_t editor_picker_bounds(const window_t* w, int absolute) {
    int width = w->w - 40;
    int height = w->h - TITLE_H - 40;
    if (width > 430) width = 430;
    if (height > 300) height = 300;
    if (width < 220) width = 220;
    if (height < 100) height = 100;
    return make_rect((w->w - width) / 2 + (absolute ? w->x : 0),
        20 + (absolute ? w->y + TITLE_H : 0), width, height);
}

static void editor_reset_view(editor_window_state_t* editor) {
    editor->row = 0;
    editor->column = 0;
    editor->top = 0;
    editor->hscroll = 0;
    editor->anchor_row = 0;
    editor->anchor_column = 0;
    editor->selection_active = 0;
    editor->caret_visible = 1;
}

static void editor_new_document(editor_window_state_t* editor) {
    editor_model_destroy(&editor->model);
    editor_model_init(&editor->model, "");
    editor_reset_view(editor);
    editor_set_status(editor, "New document - use Save As");
}

static void editor_open_document(editor_window_state_t* editor,
                                 const char* path) {
    editor_model_destroy(&editor->model);
    editor_model_init(&editor->model, path);
    editor_reset_view(editor);
    if (!editor_model_load(&editor->model))
        editor_set_status(editor, "Unable to load file");
    else
        editor_set_status(editor, "Opened");
}

static void editor_show_picker(window_t* w, gui_file_picker_mode_t mode,
                               int after_save_action) {
    editor_window_state_t* editor = EDITOR_STATE(w);
    gui_file_picker_init(&editor->picker, mode, editor->model.path);
    editor->picker_active = 1;
    editor->picker_after_save_action = after_save_action;
    w->focused_widget = EDITOR_WIDGET_PICKER;
    w->pressed_widget = 0;
}

static int editor_position_before(uint32_t ar, uint32_t ac,
                                  uint32_t br, uint32_t bc) {
    return ar < br || (ar == br && ac < bc);
}

static int editor_has_selection(const editor_window_state_t* editor) {
    return editor->selection_active &&
        (editor->anchor_row != editor->row ||
         editor->anchor_column != editor->column);
}

static void editor_selection_bounds(const editor_window_state_t* editor,
                                    uint32_t* first_row,
                                    uint32_t* first_column,
                                    uint32_t* last_row,
                                    uint32_t* last_column) {
    if (editor_position_before(editor->anchor_row, editor->anchor_column,
                               editor->row, editor->column)) {
        *first_row = editor->anchor_row;
        *first_column = editor->anchor_column;
        *last_row = editor->row;
        *last_column = editor->column;
    } else {
        *first_row = editor->row;
        *first_column = editor->column;
        *last_row = editor->anchor_row;
        *last_column = editor->anchor_column;
    }
}

static void editor_clear_selection(editor_window_state_t* editor) {
    editor->selection_active = 0;
    editor->anchor_row = editor->row;
    editor->anchor_column = editor->column;
}

static int editor_delete_selection(editor_window_state_t* editor) {
    uint32_t first_row, first_column, last_row, last_column;
    if (!editor_has_selection(editor)) return 0;
    editor_selection_bounds(editor, &first_row, &first_column,
                            &last_row, &last_column);
    if (!editor_model_delete_range(&editor->model, first_row, first_column,
                                   last_row, last_column)) return 0;
    editor->row = first_row;
    editor->column = first_column;
    editor_clear_selection(editor);
    return 1;
}

static int editor_copy_selection(editor_window_state_t* editor) {
    uint32_t first_row, first_column, last_row, last_column;
    unsigned int used = 0;
    if (!editor_has_selection(editor)) return 0;
    editor_selection_bounds(editor, &first_row, &first_column,
                            &last_row, &last_column);
    for (uint32_t row = first_row; row <= last_row; row++) {
        uint32_t start = row == first_row ? first_column : 0u;
        uint32_t end = row == last_row ? last_column :
            editor_model_line_length(&editor->model, row);
        const char* line = editor->model.lines[row];
        for (uint32_t column = start;
             column < end && used + 1u < GUI_CLIPBOARD_CAPACITY; column++)
            g_editor_clipboard[used++] = line[column];
        if (row != last_row && used + 1u < GUI_CLIPBOARD_CAPACITY)
            g_editor_clipboard[used++] = '\n';
    }
    g_editor_clipboard[used] = '\0';
    return 1;
}

static int editor_paste(editor_window_state_t* editor) {
    int changed = 0;
    if (!g_editor_clipboard[0]) return 0;
    (void)editor_delete_selection(editor);
    for (unsigned int i = 0; g_editor_clipboard[i]; i++) {
        if (g_editor_clipboard[i] == '\n') {
            if (!editor_model_split_line(&editor->model, editor->row,
                                         editor->column)) break;
            editor->row++;
            editor->column = 0;
            changed = 1;
        } else {
            if (!editor_model_insert_char(&editor->model, editor->row,
                                          editor->column,
                                          g_editor_clipboard[i])) break;
            editor->column++;
            changed = 1;
        }
    }
    editor_clear_selection(editor);
    return changed;
}

static int editor_visible_rows(const window_t* w) {
    int rows = (w->h - TITLE_H - EDITOR_TOOLBAR_H - EDITOR_STATUS_H - 4) /
               EDITOR_LINE_H;
    return rows < 1 ? 1 : rows;
}

static int editor_visible_cols(const window_t* w) {
    int cols = (w->w - 18) / 6;
    return cols < 1 ? 1 : cols;
}

static void editor_clamp_view(window_t* w) {
    editor_window_state_t* editor = EDITOR_STATE(w);
    uint32_t len;
    int rows = editor_visible_rows(w);
    int cols = editor_visible_cols(w);
    if (editor->model.count == 0) {
        editor->row = 0;
        editor->column = 0;
    } else if (editor->row >= editor->model.count) {
        editor->row = editor->model.count - 1u;
    }
    len = editor_model_line_length(&editor->model, editor->row);
    if (editor->column > len) editor->column = len;
    if (editor->row < editor->top) editor->top = editor->row;
    if (editor->row >= editor->top + (uint32_t)rows)
        editor->top = editor->row - (uint32_t)rows + 1u;
    if (editor->column < editor->hscroll) editor->hscroll = editor->column;
    if (editor->column >= editor->hscroll + (uint32_t)cols)
        editor->hscroll = editor->column - (uint32_t)cols + 1u;
}

static gui_rect_t editor_confirm_button(const window_t* w, int choice) {
    int panel_w = 244;
    int panel_x = w->x + (w->w - panel_w) / 2;
    int panel_y = w->y + TITLE_H + (w->h - TITLE_H - 74) / 2;
    return make_rect(panel_x + 8 + (choice - 1) * 76,
                     panel_y + 42, 68, 20);
}

static void editor_app_open(gui_app_context_t* context, const char* argument) {
    window_t* w = context->window;
    editor_window_state_t* editor = EDITOR_STATE(w);
    editor_model_init(&editor->model, argument ? argument : "");
    editor->caret_visible = 1;
    editor->confirm_choice = EDITOR_WIDGET_SAVE;
    editor->next_blink = sys_get_ticks() + SMALLOS_TIMER_HZ / 2u;
    if (!editor_model_load(&editor->model))
        editor_set_status(editor, "Unable to load file");
    else
        editor_set_status(editor, "F2 or Ctrl+S saves");
}

static void editor_app_close(gui_app_context_t* context) {
    window_t* w = context->window;
    if (w->state) editor_model_destroy(&EDITOR_STATE(w)->model);
}

static void editor_app_draw(gfx_surface_t* s, gui_app_context_t* context,
                            int mx, int my) {
    window_t* w = context->window;
    editor_window_state_t* editor = EDITOR_STATE(w);
    int by = w->y + TITLE_H;
    int rows = editor_visible_rows(w);
    int cols = editor_visible_cols(w);
    int text_y = by + EDITOR_TOOLBAR_H + 2;
    char location[64];
    char number[16];
    (void)mx;
    (void)my;
    fillr(s, w->x, by, w->w, w->h - TITLE_H, 0x00FFFFFFu);
    for (int widget = EDITOR_WIDGET_NEW;
         widget <= EDITOR_WIDGET_SAVE_AS; widget++) {
        const char* label = widget == EDITOR_WIDGET_NEW ? "New" :
                            widget == EDITOR_WIDGET_OPEN ? "Open" : "Save As";
        gui_widget_button(s, editor_toolbar_button(w, widget, 1), label,
            (gui_widget_state_t){w->focused_widget == widget,
                w->pressed_widget == widget, 0, 0},
            &GUI_WIDGET_THEME, draw_text);
    }
    draw_fixed_text(s, w->x + 158, by + 8,
                    editor->model.path[0] ? editor->model.path : "Untitled",
                    cols > 26 ? cols - 26 : 1, COL_TEXT);
    hline(s, w->x, by + EDITOR_TOOLBAR_H, w->w, COL_FRAME);
    for (int i = 0; i < rows; i++) {
        uint32_t row = editor->top + (uint32_t)i;
        uint32_t selection_start = 0, selection_end = 0;
        int row_selected = 0;
        if (row >= editor->model.count) break;
        if (editor_has_selection(editor)) {
            uint32_t first_row, first_column, last_row, last_column;
            editor_selection_bounds(editor, &first_row, &first_column,
                                    &last_row, &last_column);
            if (row >= first_row && row <= last_row) {
                uint32_t line_len = editor_model_line_length(&editor->model,
                                                              row);
                selection_start = row == first_row ? first_column : 0u;
                selection_end = row == last_row ? last_column : line_len + 1u;
                row_selected = selection_end > selection_start;
                if (row_selected) {
                    uint32_t visible_start = selection_start > editor->hscroll ?
                        selection_start : editor->hscroll;
                    uint32_t visible_end = selection_end < editor->hscroll +
                        (uint32_t)cols ? selection_end :
                        editor->hscroll + (uint32_t)cols;
                    if (visible_end > visible_start)
                        fillr(s, w->x + 4 +
                              (int)(visible_start - editor->hscroll) * 6,
                              text_y + i * EDITOR_LINE_H,
                              (int)(visible_end - visible_start) * 6,
                              7, COL_HILIGHT);
                }
            }
        }
        draw_fixed_text(s, w->x + 4, text_y + i * EDITOR_LINE_H,
                        editor->model.lines[row] +
                            (editor->hscroll < editor_model_line_length(
                                &editor->model, row) ? editor->hscroll :
                                editor_model_line_length(&editor->model, row)),
                        cols, COL_TEXT);
        if (row_selected) {
            uint32_t line_len = editor_model_line_length(&editor->model, row);
            uint32_t visible_start = selection_start > editor->hscroll ?
                selection_start : editor->hscroll;
            uint32_t visible_end = selection_end < editor->hscroll +
                (uint32_t)cols ? selection_end : editor->hscroll +
                (uint32_t)cols;
            if (visible_end > line_len) visible_end = line_len;
            for (uint32_t column = visible_start; column < visible_end;
                 column++)
                draw_char(s, w->x + 4 +
                          (int)(column - editor->hscroll) * 6,
                          text_y + i * EDITOR_LINE_H,
                          editor->model.lines[row][column], COL_HILIGHT_T);
        }
    }
    if (editor->caret_visible && !editor->confirm_close &&
        editor->row >= editor->top &&
        editor->row < editor->top + (uint32_t)rows) {
        int caret_col = (int)(editor->column - editor->hscroll);
        if (caret_col >= 0 && caret_col < cols)
            vline(s, w->x + 4 + caret_col * 6,
                  text_y + ((int)editor->row - (int)editor->top) * EDITOR_LINE_H,
                  7, COL_TITLE_BG);
    }
    hline(s, w->x, w->y + w->h - EDITOR_STATUS_H, w->w, COL_FRAME);
    draw_text(s, w->x + 4, w->y + w->h - 10,
              editor->model.dirty ? "*" : " ", COL_TEXT);
    u_strcpy_n(location, "Ln ", sizeof(location));
    utoa10(editor->row + 1u, number);
    u_strcat_n(location, number, sizeof(location));
    u_strcat_n(location, " Col ", sizeof(location));
    utoa10(editor->column + 1u, number);
    u_strcat_n(location, number, sizeof(location));
    u_strcat_n(location, "  ", sizeof(location));
    u_strcat_n(location, editor->status, sizeof(location));
    draw_text(s, w->x + 16, w->y + w->h - 10, location, COL_SUBTEXT);
    gui_widget_scrollbar(s,
        make_rect(w->x + w->w - 10, text_y, 10, rows * EDITOR_LINE_H),
        (int)editor->model.count, rows, (int)editor->top,
        (gui_widget_state_t){0,
            w->pressed_widget == EDITOR_WIDGET_SCROLLBAR, 0, 0},
        &GUI_WIDGET_THEME);

    if (editor->confirm_close) {
        int panel_w = 244;
        int panel_x = w->x + (w->w - panel_w) / 2;
        int panel_y = w->y + TITLE_H + (w->h - TITLE_H - 74) / 2;
        fillr(s, panel_x, panel_y, panel_w, 74, COL_WIN_BG);
        rect(s, panel_x, panel_y, panel_w, 74, COL_FRAME);
        draw_text(s, panel_x + 8, panel_y + 10,
                  "Save changes before closing?", COL_TEXT);
        for (int choice = 1; choice <= 3; choice++) {
            gui_widget_state_t state = {w->focused_widget == choice,
                w->pressed_widget == choice,
                editor->confirm_choice == choice, 0};
            const char* label = choice == 1 ? "Save" :
                                choice == 2 ? "Discard" : "Cancel";
            gui_widget_button(s, editor_confirm_button(w, choice), label,
                              state, &GUI_WIDGET_THEME, draw_text);
        }
    }
    if (editor->picker_active) {
        gui_file_picker_draw(s, &editor->picker,
            editor_picker_bounds(w, 1), &GUI_WIDGET_THEME,
            &EDITOR_PICKER_STYLE, draw_text);
    }
    rect(s, w->x, w->y, w->w, w->h, COL_FRAME);
}

static unsigned int editor_execute_action(window_t* w, int action) {
    editor_window_state_t* editor = EDITOR_STATE(w);
    editor->pending_action = EDITOR_PENDING_NONE;
    editor->confirm_close = 0;
    if (action == EDITOR_PENDING_CLOSE)
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_CLOSE;
    if (action == EDITOR_PENDING_NEW) {
        editor_new_document(editor);
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (action == EDITOR_PENDING_OPEN) {
        editor_show_picker(w, GUI_FILE_PICKER_OPEN, EDITOR_PENDING_NONE);
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    return GUI_APP_RESULT_HANDLED;
}

static unsigned int editor_request_action(window_t* w, int action) {
    editor_window_state_t* editor = EDITOR_STATE(w);
    if (!editor->model.dirty) return editor_execute_action(w, action);
    editor->pending_action = action;
    editor->confirm_close = 1;
    editor->confirm_choice = EDITOR_WIDGET_SAVE;
    w->focused_widget = EDITOR_WIDGET_SAVE;
    return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW |
           GUI_APP_RESULT_KEEP_OPEN;
}

static unsigned int editor_picker_event(window_t* w,
                                         const gui_app_event_t* event) {
    editor_window_state_t* editor = EDITOR_STATE(w);
    gui_file_picker_result_t result = gui_file_picker_event(
        &editor->picker, event, editor_picker_bounds(w, 0));
    if (editor->picker.pressed)
        w->pressed_widget = EDITOR_WIDGET_PICKER;
    else if (w->pressed_widget == EDITOR_WIDGET_PICKER)
        w->pressed_widget = 0;
    if (result == GUI_FILE_PICKER_RESULT_CANCEL) {
        editor->picker_active = 0;
        editor->picker_after_save_action = EDITOR_PENDING_NONE;
        editor->pending_action = EDITOR_PENDING_NONE;
        w->focused_widget = 0;
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (result == GUI_FILE_PICKER_RESULT_ACCEPT) {
        const char* path = gui_file_picker_path(&editor->picker);
        if (editor->picker.mode == GUI_FILE_PICKER_OPEN) {
            editor_open_document(editor, path);
            editor->picker_active = 0;
            w->focused_widget = 0;
            return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
        }
        u_strcpy_n(editor->model.path, path, sizeof(editor->model.path));
        if (!editor_model_save(&editor->model)) {
            editor_set_status(editor, "Save failed");
            return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
        }
        {
            int after_save = editor->picker_after_save_action;
            editor->picker_active = 0;
            editor->picker_after_save_action = EDITOR_PENDING_NONE;
            w->focused_widget = 0;
            editor_set_status(editor, "Saved");
            if (after_save != EDITOR_PENDING_NONE)
                return editor_execute_action(w, after_save);
        }
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    return result == GUI_FILE_PICKER_RESULT_REDRAW
        ? GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW
        : GUI_APP_RESULT_HANDLED;
}

static unsigned int editor_apply_confirm_choice(window_t* w, int choice) {
    editor_window_state_t* editor = EDITOR_STATE(w);
    int action = editor->pending_action;
    if (choice == EDITOR_WIDGET_SAVE) {
        if (!editor->model.path[0]) {
            editor->confirm_close = 0;
            editor_show_picker(w, GUI_FILE_PICKER_SAVE, action);
            return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW |
                   GUI_APP_RESULT_KEEP_OPEN;
        }
        if (editor_model_save(&editor->model))
            return editor_execute_action(w, action);
        editor_set_status(editor, "Save failed");
        editor->confirm_close = 0;
        editor->pending_action = EDITOR_PENDING_NONE;
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW |
               GUI_APP_RESULT_KEEP_OPEN;
    }
    if (choice == EDITOR_WIDGET_DISCARD) {
        editor->model.dirty = 0;
        return editor_execute_action(w, action);
    }
    editor->confirm_close = 0;
    editor->pending_action = EDITOR_PENDING_NONE;
    w->focused_widget = 0;
    return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW |
           GUI_APP_RESULT_KEEP_OPEN;
}

static unsigned int editor_confirm_event(window_t* w,
                                         const gui_app_event_t* event) {
    editor_window_state_t* editor = EDITOR_STATE(w);
    if (event->type == GUI_APP_EVENT_CLOSE_REQUEST)
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_KEEP_OPEN;
    if (event->type == GUI_APP_EVENT_POINTER_MOVE) {
        int sx = w->x + event->x;
        int sy = w->y + TITLE_H + event->y;
        int hovered = 0;
        for (int choice = 1; choice <= 3; choice++) {
            if (gui_widget_hit(editor_confirm_button(w, choice), sx, sy))
                hovered = choice;
        }
        if (hovered != w->focused_widget) {
            w->focused_widget = hovered;
            return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
        }
        return GUI_APP_RESULT_HANDLED;
    }
    if (event->type == GUI_APP_EVENT_POINTER_DOWN) {
        int sx = w->x + event->x;
        int sy = w->y + TITLE_H + event->y;
        for (int choice = 1; choice <= 3; choice++) {
            if (gui_widget_hit(editor_confirm_button(w, choice), sx, sy)) {
                editor->confirm_choice = choice;
                w->focused_widget = choice;
                w->pressed_widget = choice;
                break;
            }
        }
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_POINTER_UP) {
        int sx = w->x + event->x;
        int sy = w->y + TITLE_H + event->y;
        int pressed = w->pressed_widget;
        w->pressed_widget = 0;
        if (pressed >= 1 && pressed <= 3 &&
            gui_widget_hit(editor_confirm_button(w, pressed), sx, sy)) {
            editor->confirm_choice = pressed;
            return editor_apply_confirm_choice(w, pressed);
        }
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type != GUI_APP_EVENT_KEY)
        return GUI_APP_RESULT_HANDLED;
    if (event->key == KEY_ESC) {
        return editor_apply_confirm_choice(w, EDITOR_WIDGET_CANCEL);
    }
    if (event->key == KEY_LEFT || event->key == KEY_UP) {
        editor->confirm_choice--;
        if (editor->confirm_choice < 1) editor->confirm_choice = 3;
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->key == KEY_RIGHT || event->key == KEY_DOWN ||
        event->key == KEY_TAB) {
        editor->confirm_choice++;
        if (editor->confirm_choice > 3) editor->confirm_choice = 1;
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->key == KEY_ENTER) {
        return editor_apply_confirm_choice(w, editor->confirm_choice);
    }
    return GUI_APP_RESULT_HANDLED;
}

static void editor_pointer_position(window_t* w, int x, int y,
                                    uint32_t* row, uint32_t* column) {
    editor_window_state_t* editor = EDITOR_STATE(w);
    int text_y = EDITOR_TOOLBAR_H + 2;
    int visual_row = (y - text_y) / EDITOR_LINE_H;
    if (y < text_y) visual_row = 0;
    if (visual_row >= editor_visible_rows(w))
        visual_row = editor_visible_rows(w) - 1;
    *row = editor->top + (uint32_t)visual_row;
    if (editor->model.count == 0) *row = 0;
    else if (*row >= editor->model.count) *row = editor->model.count - 1u;
    *column = editor->hscroll +
        (uint32_t)((x > 4 ? x - 4 : 0) / 6);
    if (*column > editor_model_line_length(&editor->model, *row))
        *column = editor_model_line_length(&editor->model, *row);
}

static unsigned int editor_app_event(gui_app_context_t* context,
                                     const gui_app_event_t* event) {
    window_t* w = context->window;
    editor_window_state_t* editor = EDITOR_STATE(w);
    uint32_t len;
    int selection_deleted = 0;
    int rows = editor_visible_rows(w);
    gui_rect_t scroll_track = make_rect(w->w - 10,
        EDITOR_TOOLBAR_H + 2, 10, rows * EDITOR_LINE_H);
    gui_rect_t scroll_thumb = gui_widget_scroll_thumb(scroll_track,
        (int)editor->model.count, rows, (int)editor->top);
    if (editor->picker_active) {
        if (event->type == GUI_APP_EVENT_CLOSE_REQUEST) {
            editor->picker_active = 0;
            editor->picker_after_save_action = EDITOR_PENDING_NONE;
            editor->pending_action = EDITOR_PENDING_NONE;
            return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW |
                   GUI_APP_RESULT_KEEP_OPEN;
        }
        return editor_picker_event(w, event);
    }
    if (editor->confirm_close) return editor_confirm_event(w, event);
    if (event->type == GUI_APP_EVENT_CLOSE_REQUEST) {
        return editor_request_action(w, EDITOR_PENDING_CLOSE);
    }
    if (event->type == GUI_APP_EVENT_TICK) {
        editor->caret_visible = !editor->caret_visible;
        if (editor->row >= editor->top &&
            editor->row < editor->top + (uint32_t)rows &&
            editor->column >= editor->hscroll) {
            int caret_x = 4 +
                (int)(editor->column - editor->hscroll) * 6;
            int caret_y = EDITOR_TOOLBAR_H + 2 +
                (int)(editor->row - editor->top) * EDITOR_LINE_H;
            gui_app_invalidate(context, caret_x, caret_y, 7, EDITOR_LINE_H);
        }
        return GUI_APP_RESULT_HANDLED;
    }
    if (event->type == GUI_APP_EVENT_RESIZE) {
        editor_clamp_view(w);
        return GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_WHEEL) {
        int max_top = (int)editor->model.count - rows;
        int top = (int)editor->top - event->wheel * 3;
        if (max_top < 0) max_top = 0;
        if (top < 0) top = 0;
        if (top > max_top) top = max_top;
        editor->top = (uint32_t)top;
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_POINTER_MOVE &&
        w->pressed_widget == EDITOR_WIDGET_SCROLLBAR) {
        editor->top = (uint32_t)gui_widget_scroll_offset(
            scroll_track, (int)editor->model.count, rows, event->y,
            editor->scroll_drag_offset);
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_POINTER_MOVE &&
        w->pressed_widget == EDITOR_WIDGET_TEXT) {
        editor_pointer_position(w, event->x, event->y,
                                &editor->row, &editor->column);
        editor->selection_active = 1;
        editor->caret_visible = 1;
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_POINTER_UP &&
        (w->pressed_widget == EDITOR_WIDGET_SCROLLBAR ||
         w->pressed_widget == EDITOR_WIDGET_TEXT)) {
        if (w->pressed_widget == EDITOR_WIDGET_TEXT &&
            !editor_has_selection(editor)) editor_clear_selection(editor);
        w->pressed_widget = 0;
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_POINTER_UP &&
        w->pressed_widget >= EDITOR_WIDGET_NEW &&
        w->pressed_widget <= EDITOR_WIDGET_SAVE_AS) {
        int widget = w->pressed_widget;
        w->pressed_widget = 0;
        if (gui_widget_hit(editor_toolbar_button(w, widget, 0),
                           event->x, event->y)) {
            if (widget == EDITOR_WIDGET_NEW)
                return editor_request_action(w, EDITOR_PENDING_NEW);
            if (widget == EDITOR_WIDGET_OPEN)
                return editor_request_action(w, EDITOR_PENDING_OPEN);
            editor_show_picker(w, GUI_FILE_PICKER_SAVE, EDITOR_PENDING_NONE);
        }
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_POINTER_DOWN) {
        for (int widget = EDITOR_WIDGET_NEW;
             widget <= EDITOR_WIDGET_SAVE_AS; widget++) {
            if (gui_widget_hit(editor_toolbar_button(w, widget, 0),
                               event->x, event->y)) {
                w->focused_widget = widget;
                w->pressed_widget = widget;
                return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
            }
        }
        if (gui_widget_hit(scroll_track, event->x, event->y)) {
            if (gui_widget_hit(scroll_thumb, event->x, event->y)) {
                w->pressed_widget = EDITOR_WIDGET_SCROLLBAR;
                editor->scroll_drag_offset = event->y - scroll_thumb.y;
            } else {
                int max_top = (int)editor->model.count - rows;
                int top = (int)editor->top +
                    (event->y < scroll_thumb.y ? -rows : rows);
                if (max_top < 0) max_top = 0;
                if (top < 0) top = 0;
                if (top > max_top) top = max_top;
                editor->top = (uint32_t)top;
            }
        } else if (event->y >= EDITOR_TOOLBAR_H + 2 &&
                   event->y < EDITOR_TOOLBAR_H + 2 +
                       rows * EDITOR_LINE_H) {
            editor_pointer_position(w, event->x, event->y,
                                    &editor->row, &editor->column);
            editor->anchor_row = editor->row;
            editor->anchor_column = editor->column;
            editor->selection_active = 1;
            w->pressed_widget = EDITOR_WIDGET_TEXT;
            editor->caret_visible = 1;
        }
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type != GUI_APP_EVENT_KEY) return GUI_APP_RESULT_NONE;
    if ((event->modifiers & SYS_INPUT_KEY_CTRL) &&
        (event->key == KEY_N || event->ascii == 'n' || event->ascii == 'N'))
        return editor_request_action(w, EDITOR_PENDING_NEW);
    if ((event->modifiers & SYS_INPUT_KEY_CTRL) &&
        (event->key == KEY_O || event->ascii == 'o' || event->ascii == 'O'))
        return editor_request_action(w, EDITOR_PENDING_OPEN);
    if ((event->modifiers & SYS_INPUT_KEY_CTRL) &&
        (event->modifiers & SYS_INPUT_KEY_SHIFT) &&
        (event->key == KEY_S || event->ascii == 's' || event->ascii == 'S')) {
        editor_show_picker(w, GUI_FILE_PICKER_SAVE, EDITOR_PENDING_NONE);
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if ((event->modifiers & SYS_INPUT_KEY_CTRL) &&
        (event->key == KEY_S || event->ascii == 's' || event->ascii == 'S')) {
        if (!editor->model.path[0]) {
            editor_show_picker(w, GUI_FILE_PICKER_SAVE, EDITOR_PENDING_NONE);
            return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
        }
        editor_set_status(editor, editor_model_save(&editor->model)
            ? "Saved" : "Save failed");
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if ((event->modifiers & SYS_INPUT_KEY_CTRL) &&
        (event->key == KEY_A || event->ascii == 'a' || event->ascii == 'A')) {
        if (editor->model.count) {
            editor->anchor_row = 0;
            editor->anchor_column = 0;
            editor->row = editor->model.count - 1u;
            editor->column = editor_model_line_length(&editor->model,
                                                       editor->row);
            editor->selection_active = 1;
            editor_clamp_view(w);
        }
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if ((event->modifiers & SYS_INPUT_KEY_CTRL) &&
        (event->key == KEY_C || event->ascii == 'c' || event->ascii == 'C')) {
        editor_set_status(editor, editor_copy_selection(editor) ?
                          "Copied" : "Nothing selected");
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if ((event->modifiers & SYS_INPUT_KEY_CTRL) &&
        (event->key == KEY_X || event->ascii == 'x' || event->ascii == 'X')) {
        if (editor_copy_selection(editor) && editor_delete_selection(editor))
            editor_set_status(editor, "Cut");
        else
            editor_set_status(editor, "Nothing selected");
        editor_clamp_view(w);
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if ((event->modifiers & SYS_INPUT_KEY_CTRL) &&
        (event->key == KEY_V || event->ascii == 'v' || event->ascii == 'V')) {
        editor_set_status(editor, editor_paste(editor) ?
                          "Pasted" : "Clipboard empty");
        editor_clamp_view(w);
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    {
        int navigation = event->key == KEY_UP || event->key == KEY_DOWN ||
            event->key == KEY_LEFT || event->key == KEY_RIGHT ||
            event->key == KEY_HOME || event->key == KEY_END ||
            event->key == KEY_PAGEUP || event->key == KEY_PAGEDOWN;
        if (navigation && (event->modifiers & SYS_INPUT_KEY_SHIFT)) {
            if (!editor->selection_active) {
                editor->anchor_row = editor->row;
                editor->anchor_column = editor->column;
                editor->selection_active = 1;
            }
        } else if (navigation) {
            editor_clear_selection(editor);
        } else if (event->key == KEY_BACKSPACE || event->key == KEY_DELETE ||
                   event->key == KEY_ENTER || event->key == KEY_TAB ||
                   (event->ascii & 0xFFu) >= 32u) {
            selection_deleted = editor_delete_selection(editor);
        }
    }
    if (event->key == KEY_F2) {
        if (!editor->model.path[0]) {
            editor_show_picker(w, GUI_FILE_PICKER_SAVE, EDITOR_PENDING_NONE);
            return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
        }
        editor_set_status(editor, editor_model_save(&editor->model)
            ? "Saved" : "Save failed");
    } else if (event->key == KEY_ESC) {
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_CLOSE;
    } else if (event->key == KEY_UP) {
        if (editor->row > 0) editor->row--;
    } else if (event->key == KEY_DOWN) {
        if (editor->row + 1u < editor->model.count) editor->row++;
    } else if (event->key == KEY_LEFT) {
        if (editor->column > 0) editor->column--;
        else if (editor->row > 0) {
            editor->row--;
            editor->column = editor_model_line_length(&editor->model,
                                                       editor->row);
        }
    } else if (event->key == KEY_RIGHT) {
        len = editor_model_line_length(&editor->model, editor->row);
        if (editor->column < len) editor->column++;
        else if (editor->row + 1u < editor->model.count) {
            editor->row++;
            editor->column = 0;
        }
    } else if (event->key == KEY_HOME) {
        editor->column = 0;
    } else if (event->key == KEY_END) {
        editor->column = editor_model_line_length(&editor->model, editor->row);
    } else if (event->key == KEY_PAGEUP) {
        uint32_t amount = (uint32_t)editor_visible_rows(w);
        editor->row = amount > editor->row ? 0 : editor->row - amount;
    } else if (event->key == KEY_PAGEDOWN) {
        editor->row += (uint32_t)editor_visible_rows(w);
        if (editor->model.count && editor->row >= editor->model.count)
            editor->row = editor->model.count - 1u;
    } else if (event->key == KEY_BACKSPACE && !selection_deleted) {
        (void)editor_model_backspace(&editor->model, &editor->row,
                                     &editor->column);
    } else if (event->key == KEY_DELETE && !selection_deleted) {
        (void)editor_model_delete(&editor->model, editor->row, editor->column);
    } else if ((event->key == KEY_BACKSPACE || event->key == KEY_DELETE) &&
               selection_deleted) {
        /* The selection deletion is the complete key action. */
    } else if (event->key == KEY_ENTER) {
        if (editor_model_split_line(&editor->model, editor->row,
                                    editor->column)) {
            editor->row++;
            editor->column = 0;
        }
    } else if (event->key == KEY_TAB) {
        for (int i = 0; i < 4; i++) {
            if (!editor_model_insert_char(&editor->model, editor->row,
                                          editor->column, ' ')) break;
            editor->column++;
        }
    } else if ((event->ascii & 0xFFu) >= 32u) {
        if (editor_model_insert_char(&editor->model, editor->row,
                                     editor->column,
                                     (char)(event->ascii & 0xFFu)))
            editor->column++;
        else
            editor_set_status(editor, "Line too long");
    } else {
        return GUI_APP_RESULT_NONE;
    }
    editor->caret_visible = 1;
    editor_clamp_view(w);
    return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
}


static const gui_app_descriptor_t DESCRIPTOR = {
    "Editor", sizeof(editor_window_state_t), 560, 380, 260, 140,
    SMALLOS_TIMER_HZ / 2u, editor_app_open, editor_app_close,
    editor_app_draw, editor_app_event, GUI_APP_EDITOR, 0,
    "editor", "Editor", 1, 1
};

const gui_app_descriptor_t* gui_editor_app_descriptor(void) {
    return &DESCRIPTOR;
}
