#ifndef SMALLOS_GUI_FILE_PICKER_H
#define SMALLOS_GUI_FILE_PICKER_H

#include "app_event.h"
#include "widgets.h"

#define GUI_FILE_PICKER_PATH_MAX 256
#define GUI_FILE_PICKER_MAX_ROWS 128

typedef enum {
    GUI_FILE_PICKER_OPEN = 1,
    GUI_FILE_PICKER_SAVE,
} gui_file_picker_mode_t;

typedef enum {
    GUI_FILE_PICKER_RESULT_NONE = 0,
    GUI_FILE_PICKER_RESULT_REDRAW,
    GUI_FILE_PICKER_RESULT_ACCEPT,
    GUI_FILE_PICKER_RESULT_CANCEL,
} gui_file_picker_result_t;

typedef struct {
    gui_rect_t path;
    gui_rect_t list;
    gui_rect_t scrollbar;
    gui_rect_t filename;
    gui_rect_t accept;
    gui_rect_t cancel;
} gui_file_picker_layout_t;

typedef struct {
    unsigned int panel;
    unsigned int frame;
    unsigned int text;
    unsigned int subtext;
    unsigned int selection;
    unsigned int selected_text;
} gui_file_picker_style_t;

typedef struct {
    gui_file_picker_mode_t mode;
    char cwd[GUI_FILE_PICKER_PATH_MAX];
    char rows[GUI_FILE_PICKER_MAX_ROWS][GUI_FILE_PICKER_PATH_MAX];
    unsigned char row_is_dir[GUI_FILE_PICKER_MAX_ROWS];
    int row_count;
    int selected;
    int scroll;
    int focused;
    int pressed;
    int scroll_drag_offset;
    int last_click_row;
    uint32_t last_click_ticks;
    gui_text_input_t filename;
    char result_path[GUI_FILE_PICKER_PATH_MAX];
} gui_file_picker_t;

void gui_file_picker_init(gui_file_picker_t* picker,
                          gui_file_picker_mode_t mode,
                          const char* initial_path);
void gui_file_picker_layout(gui_rect_t bounds,
                            gui_file_picker_layout_t* layout);
void gui_file_picker_draw(gfx_surface_t* surface,
                          gui_file_picker_t* picker,
                          gui_rect_t bounds,
                          const gui_widget_theme_t* theme,
                          const gui_file_picker_style_t* style,
                          gui_widget_text_fn draw_text);
gui_file_picker_result_t gui_file_picker_event(
    gui_file_picker_t* picker, const gui_app_event_t* event,
    gui_rect_t bounds);
const char* gui_file_picker_path(const gui_file_picker_t* picker);

#endif
