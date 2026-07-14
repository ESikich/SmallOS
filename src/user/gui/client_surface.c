#include "client_surface.h"

int gui_client_surface_prepare(gfx_surface_t* desktop,
                               gui_rect_t client_screen,
                               const gui_rect_t* screen_clip,
                               gfx_surface_t* client,
                               gui_rect_t* client_clip) {
    gui_rect_t clip;
    if (!desktop || !client || !client_clip || !desktop->pixels ||
        client_screen.x < 0 || client_screen.y < 0 ||
        client_screen.w <= 0 || client_screen.h <= 0 ||
        client_screen.x >= (int)desktop->width ||
        client_screen.y >= (int)desktop->height) return 0;
    client->pixels = desktop->pixels +
        (unsigned int)client_screen.y * desktop->pitch_pixels +
        (unsigned int)client_screen.x;
    client->width = (unsigned int)client_screen.w;
    client->height = (unsigned int)client_screen.h;
    client->pitch_pixels = desktop->pitch_pixels;
    {
        gui_rect_t desktop_clip = gui_rect_make(0, 0, (int)desktop->width,
                                                (int)desktop->height);
        const gui_rect_t* requested = screen_clip ? screen_clip : &desktop_clip;
        int x0 = client_screen.x > requested->x
            ? client_screen.x : requested->x;
        int y0 = client_screen.y > requested->y
            ? client_screen.y : requested->y;
        int x1 = client_screen.x + client_screen.w <
                 requested->x + requested->w
            ? client_screen.x + client_screen.w
            : requested->x + requested->w;
        int y1 = client_screen.y + client_screen.h <
                 requested->y + requested->h
            ? client_screen.y + client_screen.h
            : requested->y + requested->h;
        clip = gui_rect_make(x0, y0, x1 > x0 ? x1 - x0 : 0,
                             y1 > y0 ? y1 - y0 : 0);
    }
    clip.x -= client_screen.x;
    clip.y -= client_screen.y;
    *client_clip = clip;
    return 1;
}

void gui_client_pointer(gui_rect_t client_screen, int screen_x, int screen_y,
                        int* client_x, int* client_y) {
    int x = screen_x - client_screen.x;
    int y = screen_y - client_screen.y;
    if (x < 0 || y < 0 || x >= client_screen.w || y >= client_screen.h)
        x = y = -1;
    if (client_x) *client_x = x;
    if (client_y) *client_y = y;
}
