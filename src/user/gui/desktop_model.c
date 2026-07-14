#include "desktop_model.h"

gui_taskbar_layout_t gui_taskbar_layout(int screen_width, int window_count) {
    gui_taskbar_layout_t layout = {0, 0, 1, 58, 0};
    int available = screen_width - 58 - 190;
    if (window_count <= 0 || available <= 0) return layout;
    layout.button_width = available / window_count;
    if (layout.button_width > 120) layout.button_width = 120;
    if (layout.button_width >= 56) {
        layout.per_page = window_count;
        return layout;
    }
    layout.paging = 1;
    layout.first_x += 20;
    available -= 40;
    layout.button_width = 56;
    layout.per_page = available / layout.button_width;
    if (layout.per_page < 1) layout.per_page = 1;
    layout.page_count = (window_count + layout.per_page - 1) / layout.per_page;
    return layout;
}

int gui_taskbar_clamp_page(gui_taskbar_layout_t layout, int page) {
    if (page < 0) return 0;
    if (page >= layout.page_count) return layout.page_count - 1;
    return page;
}

int gui_start_move_selection(int selection, int count, int delta) {
    if (count <= 0) return 0;
    selection = (selection + delta) % count;
    if (selection < 0) selection += count;
    return selection;
}

int gui_start_first_letter(const char* const* labels, int count, char letter) {
    if (letter >= 'a' && letter <= 'z') letter = (char)(letter - 'a' + 'A');
    for (int i = 0; i < count; i++) {
        char first = labels[i] && labels[i][0] ? labels[i][0] : 0;
        if (first >= 'a' && first <= 'z') first = (char)(first - 'a' + 'A');
        if (first == letter) return i;
    }
    return -1;
}
