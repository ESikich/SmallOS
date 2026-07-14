#include "cursor.h"

gui_rect_t gui_cursor_rect(int x, int y) {
    return gui_rect_make(x, y, GUI_CURSOR_WIDTH, GUI_CURSOR_HEIGHT);
}

void gui_cursor_draw(gfx_surface_t* surface, int x, int y) {
    static const unsigned char arrow[GUI_CURSOR_HEIGHT][GUI_CURSOR_WIDTH] = {
        {1,0,0,0,0,0,0,0,0}, {1,1,0,0,0,0,0,0,0},
        {1,2,1,0,0,0,0,0,0}, {1,2,2,1,0,0,0,0,0},
        {1,2,2,2,1,0,0,0,0}, {1,2,2,2,2,1,0,0,0},
        {1,2,2,2,2,2,1,0,0}, {1,2,2,2,2,2,2,1,0},
        {1,2,2,2,1,1,1,1,0}, {1,2,1,2,2,1,0,0,0},
        {1,1,0,1,2,2,1,0,0}, {0,0,0,0,1,2,1,0,0},
    };
    for (int row = 0; row < GUI_CURSOR_HEIGHT; row++) {
        for (int column = 0; column < GUI_CURSOR_WIDTH; column++) {
            if (arrow[row][column] == 1)
                gui_canvas_put_pixel(surface, x + column, y + row, 0);
            else if (arrow[row][column] == 2)
                gui_canvas_put_pixel(surface, x + column, y + row, 0xFFFFFFu);
        }
    }
}
