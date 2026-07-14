#ifndef SMALLOS_GUI_NATIVE_APPS_INTERNAL_H
#define SMALLOS_GUI_NATIVE_APPS_INTERNAL_H

#include "native_apps.h"
#include "network_model.h"
#include "tasks_model.h"
#include "viewer_model.h"
#include "../image_bmp.h"
#include "keyboard.h"
#include "signal.h"
#include "unistd.h"
#include "user_lib.h"

#define TASK_ROW_H 13
#define TASK_MAX SYS_PROCINFO_MAX

extern gui_native_ui_t g_ui;
extern char g_pref_address[32];
extern char g_pref_prefix[8];
extern char g_pref_gateway[32];
extern char g_pref_dns[32];

int native_inside(int x, int y, gui_rect_t rectangle);
void native_copy_text(char* destination, const char* source,
                      unsigned int capacity);
void native_append_text(char* destination, const char* source,
                        unsigned int capacity);
void native_uint_text(unsigned int value, char* out);
void native_draw_value(gfx_surface_t* surface, int x, int y,
                       unsigned int value);

#define inside native_inside
#define copy_text native_copy_text
#define append_text native_append_text
#define uint_text native_uint_text
#define draw_value native_draw_value

#endif
