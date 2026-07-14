#include "occlusion.h"

int gui_visible_regions(gui_rect_t source,
                        const gui_rect_t* opaque,
                        int opaque_count,
                        gui_rect_t* output,
                        int output_capacity) {
    gui_rect_t current[GUI_VISIBLE_REGION_CAPACITY];
    gui_rect_t next[GUI_VISIBLE_REGION_CAPACITY];
    int current_count = 1;
    if (!output || output_capacity <= 0 || gui_rect_empty(source)) return 0;
    current[0] = source;
    for (int cut = 0; cut < opaque_count; cut++) {
        int next_count = 0;
        for (int i = 0; i < current_count; i++) {
            gui_rect_t pieces[4];
            int count = gui_rect_exclude(current[i], opaque[cut], pieces);
            if (next_count + count > GUI_VISIBLE_REGION_CAPACITY) return -1;
            for (int p = 0; p < count; p++) next[next_count++] = pieces[p];
        }
        current_count = next_count;
        for (int i = 0; i < current_count; i++) current[i] = next[i];
        if (!current_count) break;
    }
    if (current_count > output_capacity) return -1;
    for (int i = 0; i < current_count; i++) output[i] = current[i];
    return current_count;
}
