#ifndef SMALLOS_GUI_APP_SERVICES_H
#define SMALLOS_GUI_APP_SERVICES_H

#include "builtin_apps.h"
#include "framework.h"
#include "widgets.h"

typedef struct gui_app_perf_snapshot {
    unsigned int composed_pixels;
    unsigned int presented_pixels;
    unsigned int dirty_regions;
    unsigned int full_repaints;
    unsigned int idle_wakeups;
    unsigned int pty_wakeups;
} gui_app_perf_snapshot_t;

void gui_app_services_draw_text(gfx_surface_t* surface, int x, int y,
                                const char* text, unsigned int color);
const gui_builtin_style_t* gui_app_services_builtin_style(void);
const gui_widget_theme_t* gui_app_services_widget_theme(void);
int gui_app_services_performance_visible(void);
void gui_app_services_performance_snapshot(gui_app_perf_snapshot_t* snapshot);
void gui_app_services_toggle_performance(void);
/* Returns 1 for Editor, 2 for Viewer, 3 for compatibility handoff, or 0. */
int gui_app_services_open_path(gui_app_context_t* context, const char* path);

#endif
