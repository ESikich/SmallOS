#ifndef SMALLOS_GUI_VIEWER_MODEL_H
#define SMALLOS_GUI_VIEWER_MODEL_H

typedef struct gui_viewer_geometry {
    int width;
    int height;
    int pan_x;
    int pan_y;
} gui_viewer_geometry_t;

void gui_viewer_scaled_size(unsigned int source_width,
                            unsigned int source_height,
                            int viewport_width,
                            int viewport_height,
                            int zoom_percent,
                            int* output_width,
                            int* output_height);
void gui_viewer_clamp_pan(gui_viewer_geometry_t* geometry,
                          int viewport_width,
                          int viewport_height);

#endif
