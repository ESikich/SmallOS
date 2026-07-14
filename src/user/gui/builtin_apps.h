#ifndef SMALLOS_GUI_BUILTIN_APPS_H
#define SMALLOS_GUI_BUILTIN_APPS_H

#include "widgets.h"

typedef struct {
    unsigned int background;
    unsigned int frame;
    unsigned int text;
    unsigned int subtext;
} gui_builtin_style_t;

void gui_builtin_draw_system(gfx_surface_t* surface, gui_rect_t bounds,
                             const gui_builtin_style_t* style,
                             gui_widget_text_fn draw_text);
void gui_builtin_draw_config(gfx_surface_t* surface, gui_rect_t bounds,
                             int perf_visible,
                             gui_widget_state_t control_state,
                             const gui_builtin_style_t* style,
                             const gui_widget_theme_t* theme,
                             gui_widget_text_fn draw_text);
void gui_builtin_draw_about(gfx_surface_t* surface, gui_rect_t bounds,
                            const gui_builtin_style_t* style,
                            gui_widget_text_fn draw_text);

#endif
