#include "editor_app.h"
#include "app_services.h"
#include "canvas.h"
#include "keyboard.h"
#include "user_lib.h"
#include "widgets.h"
#include "../editor_model.h"

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

typedef struct {
    editor_model_t model;
    uint32_t row;
    uint32_t column;
    uint32_t top;
    uint32_t hscroll;
    uint32_t next_blink;
    int caret_visible;
    uint32_t anchor_row;
    uint32_t anchor_column;
    int selection_active;
    int scroll_drag_offset;
    int pending_action;
    int picker_mode;
    int picker_after_save_action;
    char status[80];
} editor_window_state_t;

static editor_window_state_t* editor_state(gui_app_context_t* context) {
    return (editor_window_state_t*)gui_app_state(context);
}
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
    char clipped[256];
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
#define EDITOR_DIALOG_DIRTY 1u
#define EDITOR_BUTTON_SAVE 1u
#define EDITOR_BUTTON_DISCARD 2u
#define EDITOR_BUTTON_CANCEL 3u
#define EDITOR_WIDGET_SCROLLBAR 10
#define EDITOR_WIDGET_TEXT 11
#define EDITOR_WIDGET_NEW 20
#define EDITOR_WIDGET_OPEN 21
#define EDITOR_WIDGET_SAVE_AS 22
#define EDITOR_COMMAND_SAVE 23

static const gui_command_t EDITOR_COMMANDS[] = {
    {EDITOR_WIDGET_NEW, "New", KEY_N, SYS_INPUT_KEY_CTRL, 1, 0},
    {EDITOR_WIDGET_OPEN, "Open", KEY_O, SYS_INPUT_KEY_CTRL, 1, 0},
    {EDITOR_WIDGET_SAVE_AS, "Save As", KEY_S,
        SYS_INPUT_KEY_CTRL | SYS_INPUT_KEY_SHIFT, 1, 0},
    {EDITOR_COMMAND_SAVE, "Save", KEY_S, SYS_INPUT_KEY_CTRL, 1, 0},
};

static const gui_command_t* editor_command(unsigned int id) {
    for (unsigned int i = 0;
         i < sizeof(EDITOR_COMMANDS) / sizeof(EDITOR_COMMANDS[0]); i++)
        if (EDITOR_COMMANDS[i].id == id) return &EDITOR_COMMANDS[i];
    return 0;
}
#define EDITOR_PENDING_NONE 0
#define EDITOR_PENDING_CLOSE 1
#define EDITOR_PENDING_NEW 2
#define EDITOR_PENDING_OPEN 3

static void editor_set_status(editor_window_state_t* editor,
                              const char* status) {
    u_strcpy_n(editor->status, status, sizeof(editor->status));
}

static gui_rect_t editor_toolbar_button(int widget) {
    int x = widget == EDITOR_WIDGET_NEW ? 4 :
            widget == EDITOR_WIDGET_OPEN ? 48 : 92;
    int width = widget == EDITOR_WIDGET_SAVE_AS ? 60 : 40;
    return make_rect(x, 3, width, 18);
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

static void editor_show_picker(gui_app_context_t* context,
                               gui_file_request_mode_t mode,
                               int after_save_action) {
    editor_window_state_t* editor = editor_state(context);
    if (!gui_app_open_file_picker(context, mode, GUI_FILE_FILTER_TEXT,
                                  editor->model.path)) {
        editor_set_status(editor, "Another dialog is already open");
        return;
    }
    editor->picker_mode = mode;
    editor->picker_after_save_action = after_save_action;
    gui_app_release_pointer(context);
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

static int editor_visible_rows(gui_app_context_t* context) {
    int height = 0;
    gui_app_client_size(context, 0, &height);
    int rows = (height - EDITOR_TOOLBAR_H - EDITOR_STATUS_H - 4) /
               EDITOR_LINE_H;
    return rows < 1 ? 1 : rows;
}

static int editor_visible_cols(gui_app_context_t* context) {
    int width = 0;
    gui_app_client_size(context, &width, 0);
    int cols = (width - 18) / 6;
    return cols < 1 ? 1 : cols;
}

static void editor_clamp_view(gui_app_context_t* context) {
    editor_window_state_t* editor = editor_state(context);
    uint32_t len;
    int rows = editor_visible_rows(context);
    int cols = editor_visible_cols(context);
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

static void editor_app_open(gui_app_context_t* context, const char* argument) {
    editor_window_state_t* editor = editor_state(context);
    editor_model_init(&editor->model, argument ? argument : "");
    editor->caret_visible = 1;
    editor->next_blink = sys_get_ticks() + SMALLOS_TIMER_HZ / 2u;
    if (!editor_model_load(&editor->model))
        editor_set_status(editor, "Unable to load file");
    else
        editor_set_status(editor, "F2 or Ctrl+S saves");
}

static void editor_app_close(gui_app_context_t* context) {
    editor_window_state_t* editor = editor_state(context);
    if (editor) editor_model_destroy(&editor->model);
}

static void editor_app_draw(gfx_surface_t* s, gui_app_context_t* context,
                            int mx, int my) {
    editor_window_state_t* editor = editor_state(context);
    int width = (int)s->width;
    int height = (int)s->height;
    int rows = editor_visible_rows(context);
    int cols = editor_visible_cols(context);
    int text_y = EDITOR_TOOLBAR_H + 2;
    char location[64];
    char number[16];
    (void)mx;
    (void)my;
    fillr(s, 0, 0, width, height, 0x00FFFFFFu);
    for (int widget = EDITOR_WIDGET_NEW;
         widget <= EDITOR_WIDGET_SAVE_AS; widget++) {
        const gui_command_t* command = editor_command((unsigned int)widget);
        const char* label = command ? command->label : "";
        gui_widget_button(s, editor_toolbar_button(widget), label,
            (gui_widget_state_t){gui_app_focused_control(context) == widget,
                gui_app_captured_control(context) == widget, 0, 0},
            &GUI_WIDGET_THEME, draw_text);
    }
    draw_fixed_text(s, 158, 8,
                    editor->model.path[0] ? editor->model.path : "Untitled",
                    cols > 26 ? cols - 26 : 1, COL_TEXT);
    hline(s, 0, EDITOR_TOOLBAR_H, width, COL_FRAME);
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
                        fillr(s, 4 +
                              (int)(visible_start - editor->hscroll) * 6,
                              text_y + i * EDITOR_LINE_H,
                              (int)(visible_end - visible_start) * 6,
                              7, COL_HILIGHT);
                }
            }
        }
        draw_fixed_text(s, 4, text_y + i * EDITOR_LINE_H,
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
                draw_char(s, 4 +
                          (int)(column - editor->hscroll) * 6,
                          text_y + i * EDITOR_LINE_H,
                          editor->model.lines[row][column], COL_HILIGHT_T);
        }
    }
    if (editor->caret_visible &&
        editor->row >= editor->top &&
        editor->row < editor->top + (uint32_t)rows) {
        int caret_col = (int)(editor->column - editor->hscroll);
        if (caret_col >= 0 && caret_col < cols)
            vline(s, 4 + caret_col * 6,
                  text_y + ((int)editor->row - (int)editor->top) * EDITOR_LINE_H,
                  7, COL_TITLE_BG);
    }
    hline(s, 0, height - EDITOR_STATUS_H, width, COL_FRAME);
    draw_text(s, 4, height - 10,
              editor->model.dirty ? "*" : " ", COL_TEXT);
    u_strcpy_n(location, "Ln ", sizeof(location));
    utoa10(editor->row + 1u, number);
    u_strcat_n(location, number, sizeof(location));
    u_strcat_n(location, " Col ", sizeof(location));
    utoa10(editor->column + 1u, number);
    u_strcat_n(location, number, sizeof(location));
    u_strcat_n(location, "  ", sizeof(location));
    u_strcat_n(location, editor->status, sizeof(location));
    draw_text(s, 16, height - 10, location, COL_SUBTEXT);
    gui_widget_scrollbar(s,
        make_rect(width - 10, text_y, 10, rows * EDITOR_LINE_H),
        (int)editor->model.count, rows, (int)editor->top,
        (gui_widget_state_t){0,
            gui_app_captured_control(context) == EDITOR_WIDGET_SCROLLBAR, 0, 0},
        &GUI_WIDGET_THEME);
}

static unsigned int editor_execute_action(gui_app_context_t* context,
                                          int action) {
    editor_window_state_t* editor = editor_state(context);
    editor->pending_action = EDITOR_PENDING_NONE;
    if (action == EDITOR_PENDING_CLOSE)
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_CLOSE;
    if (action == EDITOR_PENDING_NEW) {
        editor_new_document(editor);
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (action == EDITOR_PENDING_OPEN) {
        editor_show_picker(context, GUI_FILE_REQUEST_OPEN,
                           EDITOR_PENDING_NONE);
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    return GUI_APP_RESULT_HANDLED;
}

static unsigned int editor_request_action(gui_app_context_t* context,
                                          int action) {
    static const gui_dialog_button_t buttons[] = {
        {EDITOR_BUTTON_SAVE, "Save", 1, 0},
        {EDITOR_BUTTON_DISCARD, "Discard", 0, 0},
        {EDITOR_BUTTON_CANCEL, "Cancel", 0, 1},
    };
    gui_dialog_request_t request;
    editor_window_state_t* editor = editor_state(context);
    if (!editor->model.dirty)
        return editor_execute_action(context, action);
    editor->pending_action = action;
    request.request_id = EDITOR_DIALOG_DIRTY;
    request.title = "Unsaved Changes";
    request.message = "Save changes before continuing?";
    request.initial_text = 0;
    request.buttons = buttons;
    request.button_count = 3;
    if (!gui_app_open_dialog(context, &request)) {
        editor->pending_action = EDITOR_PENDING_NONE;
        editor_set_status(editor, "Another dialog is already open");
    }
    return GUI_APP_RESULT_HANDLED |
           GUI_APP_RESULT_KEEP_OPEN;
}

static unsigned int editor_file_result(gui_app_context_t* context,
                                       const gui_app_event_t* event) {
    editor_window_state_t* editor = editor_state(context);
    if (event->type == GUI_APP_EVENT_FILE_CANCELLED) {
        editor->picker_mode = 0;
        editor->picker_after_save_action = EDITOR_PENDING_NONE;
        editor->pending_action = EDITOR_PENDING_NONE;
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_FILE_SELECTED) {
        if (editor->picker_mode == GUI_FILE_REQUEST_OPEN) {
            editor_open_document(editor, event->path);
            editor->picker_mode = 0;
            return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
        }
        u_strcpy_n(editor->model.path, event->path,
                   sizeof(editor->model.path));
        if (!editor_model_save(&editor->model)) {
            editor_set_status(editor, "Save failed");
            return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
        }
        {
            int after_save = editor->picker_after_save_action;
            editor->picker_mode = 0;
            editor->picker_after_save_action = EDITOR_PENDING_NONE;
            editor_set_status(editor, "Saved");
            if (after_save != EDITOR_PENDING_NONE)
                return editor_execute_action(context, after_save);
        }
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    return GUI_APP_RESULT_NONE;
}

static unsigned int editor_apply_confirm_choice(gui_app_context_t* context,
                                                 unsigned int choice) {
    editor_window_state_t* editor = editor_state(context);
    int action = editor->pending_action;
    if (choice == EDITOR_BUTTON_SAVE) {
        if (!editor->model.path[0]) {
            editor_show_picker(context, GUI_FILE_REQUEST_SAVE, action);
            return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW |
                   GUI_APP_RESULT_KEEP_OPEN;
        }
        if (editor_model_save(&editor->model))
            return editor_execute_action(context, action);
        editor_set_status(editor, "Save failed");
        editor->pending_action = EDITOR_PENDING_NONE;
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW |
               GUI_APP_RESULT_KEEP_OPEN;
    }
    if (choice == EDITOR_BUTTON_DISCARD) {
        editor->model.dirty = 0;
        return editor_execute_action(context, action);
    }
    editor->pending_action = EDITOR_PENDING_NONE;
    return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW |
           GUI_APP_RESULT_KEEP_OPEN;
}

static void editor_pointer_position(gui_app_context_t* context, int x, int y,
                                    uint32_t* row, uint32_t* column) {
    editor_window_state_t* editor = editor_state(context);
    int text_y = EDITOR_TOOLBAR_H + 2;
    int visual_row = (y - text_y) / EDITOR_LINE_H;
    if (y < text_y) visual_row = 0;
    if (visual_row >= editor_visible_rows(context))
        visual_row = editor_visible_rows(context) - 1;
    *row = editor->top + (uint32_t)visual_row;
    if (editor->model.count == 0) *row = 0;
    else if (*row >= editor->model.count) *row = editor->model.count - 1u;
    *column = editor->hscroll +
        (uint32_t)((x > 4 ? x - 4 : 0) / 6);
    if (*column > editor_model_line_length(&editor->model, *row))
        *column = editor_model_line_length(&editor->model, *row);
}

static unsigned int editor_run_command(gui_app_context_t* context,
                                       unsigned int command_id) {
    editor_window_state_t* editor = editor_state(context);
    if (command_id == EDITOR_WIDGET_NEW)
        return editor_request_action(context, EDITOR_PENDING_NEW);
    if (command_id == EDITOR_WIDGET_OPEN)
        return editor_request_action(context, EDITOR_PENDING_OPEN);
    if (command_id == EDITOR_WIDGET_SAVE_AS) {
        editor_show_picker(context, GUI_FILE_REQUEST_SAVE, EDITOR_PENDING_NONE);
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (command_id == EDITOR_COMMAND_SAVE) {
        if (!editor->model.path[0]) {
            editor_show_picker(context, GUI_FILE_REQUEST_SAVE,
                               EDITOR_PENDING_NONE);
            return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
        }
        editor_set_status(editor, editor_model_save(&editor->model)
            ? "Saved" : "Save failed");
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    return GUI_APP_RESULT_NONE;
}

static unsigned int editor_app_event(gui_app_context_t* context,
                                     const gui_app_event_t* event) {
    editor_window_state_t* editor = editor_state(context);
    uint32_t len;
    int selection_deleted = 0;
    int width = 0;
    int rows = editor_visible_rows(context);
    gui_app_client_size(context, &width, 0);
    gui_rect_t scroll_track = make_rect(width - 10,
        EDITOR_TOOLBAR_H + 2, 10, rows * EDITOR_LINE_H);
    gui_rect_t scroll_thumb = gui_widget_scroll_thumb(scroll_track,
        (int)editor->model.count, rows, (int)editor->top);
    if (event->type == GUI_APP_EVENT_FILE_SELECTED ||
        event->type == GUI_APP_EVENT_FILE_CANCELLED)
        return editor_file_result(context, event);
    if (event->type == GUI_APP_EVENT_DIALOG_RESULT &&
        event->request_id == EDITOR_DIALOG_DIRTY)
        return editor_apply_confirm_choice(context, event->button_id);
    if (event->type == GUI_APP_EVENT_CLOSE_REQUEST) {
        return editor_request_action(context, EDITOR_PENDING_CLOSE);
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
        editor_clamp_view(context);
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
        gui_app_captured_control(context) == EDITOR_WIDGET_SCROLLBAR) {
        editor->top = (uint32_t)gui_widget_scroll_offset(
            scroll_track, (int)editor->model.count, rows, event->y,
            editor->scroll_drag_offset);
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_POINTER_MOVE &&
        gui_app_captured_control(context) == EDITOR_WIDGET_TEXT) {
        editor_pointer_position(context, event->x, event->y,
                                &editor->row, &editor->column);
        editor->selection_active = 1;
        editor->caret_visible = 1;
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_POINTER_UP &&
        (gui_app_captured_control(context) == EDITOR_WIDGET_SCROLLBAR ||
         gui_app_captured_control(context) == EDITOR_WIDGET_TEXT)) {
        if (gui_app_captured_control(context) == EDITOR_WIDGET_TEXT &&
            !editor_has_selection(editor)) editor_clear_selection(editor);
        gui_app_release_pointer(context);
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_POINTER_UP &&
        gui_app_captured_control(context) >= EDITOR_WIDGET_NEW &&
        gui_app_captured_control(context) <= EDITOR_WIDGET_SAVE_AS) {
        int widget = gui_app_captured_control(context);
        gui_app_release_pointer(context);
        if (gui_widget_hit(editor_toolbar_button(widget),
                           event->x, event->y))
            return editor_run_command(context, (unsigned int)widget);
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_POINTER_DOWN) {
        for (int widget = EDITOR_WIDGET_NEW;
             widget <= EDITOR_WIDGET_SAVE_AS; widget++) {
            if (gui_widget_hit(editor_toolbar_button(widget),
                               event->x, event->y)) {
                gui_app_focus_control(context, widget);
                gui_app_capture_pointer(context, widget);
                return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
            }
        }
        if (gui_widget_hit(scroll_track, event->x, event->y)) {
            gui_app_focus_control(context, EDITOR_WIDGET_SCROLLBAR);
            if (gui_widget_hit(scroll_thumb, event->x, event->y)) {
                gui_app_capture_pointer(context, EDITOR_WIDGET_SCROLLBAR);
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
            gui_app_focus_control(context, EDITOR_WIDGET_TEXT);
            editor_pointer_position(context, event->x, event->y,
                                    &editor->row, &editor->column);
            editor->anchor_row = editor->row;
            editor->anchor_column = editor->column;
            editor->selection_active = 1;
            gui_app_capture_pointer(context, EDITOR_WIDGET_TEXT);
            editor->caret_visible = 1;
        }
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type != GUI_APP_EVENT_KEY) return GUI_APP_RESULT_NONE;
    if (gui_app_focused_control(context) >= EDITOR_WIDGET_NEW &&
        gui_app_focused_control(context) <= EDITOR_WIDGET_SAVE_AS) {
        if (event->key == KEY_TAB) {
            int next = gui_widget_focus_next(
                gui_app_focused_control(context) - EDITOR_WIDGET_NEW,
                EDITOR_WIDGET_SAVE_AS - EDITOR_WIDGET_NEW + 1,
                (event->modifiers & SYS_INPUT_KEY_SHIFT) != 0, 0);
            gui_app_focus_control(context, next + EDITOR_WIDGET_NEW);
            return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
        }
        if (event->key == KEY_ENTER || event->key == KEY_SPACE)
            return editor_run_command(context,
                (unsigned int)gui_app_focused_control(context));
    }
    {
        unsigned int command_id = gui_widget_command_for_key(
            EDITOR_COMMANDS,
            (int)(sizeof(EDITOR_COMMANDS) / sizeof(EDITOR_COMMANDS[0])),
            event->key, event->modifiers,
            SYS_INPUT_KEY_CTRL | SYS_INPUT_KEY_SHIFT | SYS_INPUT_KEY_ALT);
        if (command_id) return editor_run_command(context, command_id);
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
            editor_clamp_view(context);
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
        editor_clamp_view(context);
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if ((event->modifiers & SYS_INPUT_KEY_CTRL) &&
        (event->key == KEY_V || event->ascii == 'v' || event->ascii == 'V')) {
        editor_set_status(editor, editor_paste(editor) ?
                          "Pasted" : "Clipboard empty");
        editor_clamp_view(context);
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
        return editor_run_command(context, EDITOR_COMMAND_SAVE);
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
        uint32_t amount = (uint32_t)editor_visible_rows(context);
        editor->row = amount > editor->row ? 0 : editor->row - amount;
    } else if (event->key == KEY_PAGEDOWN) {
        editor->row += (uint32_t)editor_visible_rows(context);
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
    editor_clamp_view(context);
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
