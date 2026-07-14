#include "viewer_model.h"

void gui_viewer_scaled_size(unsigned int source_width,
                            unsigned int source_height,
                            int viewport_width,
                            int viewport_height,
                            int zoom_percent,
                            int* output_width,
                            int* output_height) {
    unsigned int width = source_width;
    unsigned int height = source_height;
    if (!source_width || !source_height || viewport_width <= 0 ||
        viewport_height <= 0) {
        width = height = 0;
    } else if (zoom_percent > 0) {
        width = source_width * (unsigned int)zoom_percent / 100u;
        height = source_height * (unsigned int)zoom_percent / 100u;
    } else if (width > (unsigned int)viewport_width ||
               height > (unsigned int)viewport_height) {
        if ((unsigned long long)source_width * (unsigned int)viewport_height >
            (unsigned long long)source_height * (unsigned int)viewport_width) {
            width = (unsigned int)viewport_width;
            height = (unsigned int)((unsigned long long)source_height * width /
                                    source_width);
        } else {
            height = (unsigned int)viewport_height;
            width = (unsigned int)((unsigned long long)source_width * height /
                                   source_height);
        }
    }
    if (source_width && !width) width = 1;
    if (source_height && !height) height = 1;
    if (output_width) *output_width = (int)width;
    if (output_height) *output_height = (int)height;
}

void gui_viewer_clamp_pan(gui_viewer_geometry_t* geometry,
                          int viewport_width,
                          int viewport_height) {
    int max_x;
    int max_y;
    if (!geometry) return;
    max_x = geometry->width > viewport_width
          ? (geometry->width - viewport_width + 1) / 2 : 0;
    max_y = geometry->height > viewport_height
          ? (geometry->height - viewport_height + 1) / 2 : 0;
    if (geometry->pan_x < -max_x) geometry->pan_x = -max_x;
    if (geometry->pan_x > max_x) geometry->pan_x = max_x;
    if (geometry->pan_y < -max_y) geometry->pan_y = -max_y;
    if (geometry->pan_y > max_y) geometry->pan_y = max_y;
}
