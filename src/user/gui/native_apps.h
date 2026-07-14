#ifndef SMALLOS_GUI_NATIVE_APPS_H
#define SMALLOS_GUI_NATIVE_APPS_H

#include "framework.h"
#include "widgets.h"

typedef struct gui_native_ui {
    void (*draw_text)(gfx_surface_t*, int, int, const char*, unsigned int);
    unsigned int (*text_width)(const char*);
    const gui_widget_theme_t* widget_theme;
    unsigned int window_bg;
    unsigned int frame;
    unsigned int text;
    unsigned int subtext;
    unsigned int highlight;
    unsigned int highlight_text;
} gui_native_ui_t;

void gui_native_apps_init(const gui_native_ui_t* ui);
const gui_app_descriptor_t* gui_native_app_descriptor(gui_app_id_t id);
void gui_native_network_pref_set(const char* key, const char* value);
const char* gui_native_network_pref_get(const char* key);

#endif
