#ifndef SMALLOS_GUI_DESKTOP_MODEL_H
#define SMALLOS_GUI_DESKTOP_MODEL_H

typedef struct gui_taskbar_layout {
    int button_width;
    int per_page;
    int page_count;
    int first_x;
    int paging;
} gui_taskbar_layout_t;

gui_taskbar_layout_t gui_taskbar_layout(int screen_width, int window_count);
int gui_taskbar_clamp_page(gui_taskbar_layout_t layout, int page);
int gui_start_move_selection(int selection, int count, int delta);
int gui_start_first_letter(const char* const* labels, int count, char letter);

#endif
