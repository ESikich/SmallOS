#ifndef SMALLOS_GUI_WIDGETS_H
#define SMALLOS_GUI_WIDGETS_H

#include "canvas.h"

typedef struct {
    unsigned int face;
    unsigned int face_hover;
    unsigned int face_pressed;
    unsigned int frame;
    unsigned int text;
    unsigned int disabled;
    unsigned int accent;
} gui_widget_theme_t;

typedef struct {
    int hovered;
    int pressed;
    int focused;
    int disabled;
} gui_widget_state_t;

typedef void (*gui_widget_text_fn)(gfx_surface_t* surface, int x, int y,
                                   const char* text, unsigned int color);

#define GUI_TEXT_INPUT_CAPACITY 128

typedef struct {
    char text[GUI_TEXT_INPUT_CAPACITY];
    int length;
    int cursor;
} gui_text_input_t;

typedef enum {
    GUI_TEXT_INPUT_LEFT = 1,
    GUI_TEXT_INPUT_RIGHT,
    GUI_TEXT_INPUT_HOME,
    GUI_TEXT_INPUT_END,
    GUI_TEXT_INPUT_BACKSPACE,
    GUI_TEXT_INPUT_DELETE,
} gui_text_input_command_t;

int gui_widget_hit(gui_rect_t bounds, int x, int y);
void gui_widget_label(gfx_surface_t* surface, gui_rect_t bounds,
                      const char* text, unsigned int color,
                      gui_widget_text_fn draw_text);
void gui_widget_button(gfx_surface_t* surface, gui_rect_t bounds,
                       const char* text, gui_widget_state_t state,
                       const gui_widget_theme_t* theme,
                       gui_widget_text_fn draw_text);
void gui_widget_checkbox(gfx_surface_t* surface, gui_rect_t bounds,
                         const char* text, int checked,
                         gui_widget_state_t state,
                         const gui_widget_theme_t* theme,
                         gui_widget_text_fn draw_text);
gui_rect_t gui_widget_scroll_thumb(gui_rect_t track, int total,
                                   int visible, int offset);
int gui_widget_scroll_offset(gui_rect_t track, int total, int visible,
                             int pointer_y, int grab_offset);
void gui_widget_scrollbar(gfx_surface_t* surface, gui_rect_t track,
                          int total, int visible, int offset,
                          gui_widget_state_t state,
                          const gui_widget_theme_t* theme);
void gui_widget_text_field(gfx_surface_t* surface, gui_rect_t bounds,
                           const char* text, int cursor,
                           gui_widget_state_t state,
                           const gui_widget_theme_t* theme,
                           gui_widget_text_fn draw_text);
void gui_text_input_init(gui_text_input_t* input, const char* text);
int gui_text_input_insert(gui_text_input_t* input, char ch);
int gui_text_input_command(gui_text_input_t* input,
                           gui_text_input_command_t command);

#endif
