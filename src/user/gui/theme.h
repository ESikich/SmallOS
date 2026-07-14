#ifndef SMALLOS_GUI_THEME_H
#define SMALLOS_GUI_THEME_H

#include "app_services.h"
#include "gfx.h"
#include "widgets.h"

#define COL_DESKTOP_A 0x00C0C0B0u
#define COL_DESKTOP_B 0x00B0B0A0u
#define COL_WIN_BG 0x00FFFFFFu
#define COL_FRAME 0x00000000u
#define COL_TITLE_BG 0x00000000u
#define COL_TITLE_FG 0x00FFFFFFu
#define COL_TITLE_IDLE_BG 0x00808080u
#define COL_TEXT 0x00000000u
#define COL_SUBTEXT 0x00404040u
#define COL_HILIGHT 0x000060A0u
#define COL_HILIGHT_T 0x00FFFFFFu
#define COL_BTN_BG 0x00E0E0E0u
#define COL_BAR 0x00D4D0C8u
#define COL_SHADOW 0x00606060u

extern const gui_widget_theme_t gui_retro_widget_theme;
extern const gui_builtin_style_t gui_retro_builtin_style;

void gui_theme_draw_text(gfx_surface_t* surface, int x, int y,
                         const char* text, unsigned int color);
void gui_theme_draw_fixed_text(gfx_surface_t* surface, int x, int y,
                               const char* text, int max_chars,
                               unsigned int color);
unsigned int gui_theme_text_width(const char* text);

#endif
