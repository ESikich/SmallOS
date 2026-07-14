#include "native_apps_internal.h"

gui_native_ui_t g_ui;
char g_pref_address[32];
char g_pref_prefix[8];
char g_pref_gateway[32];
char g_pref_dns[32];

int native_inside(int x, int y, gui_rect_t rectangle) {
    return x >= rectangle.x && x < rectangle.x + rectangle.w &&
           y >= rectangle.y && y < rectangle.y + rectangle.h;
}

void native_copy_text(char* destination, const char* source,
                      unsigned int capacity) {
    unsigned int i = 0;
    if (!capacity) return;
    while (source && source[i] && i + 1u < capacity) {
        destination[i] = source[i];
        i++;
    }
    destination[i] = 0;
}

void native_append_text(char* destination, const char* source,
                        unsigned int capacity) {
    unsigned int used = 0;
    while (used < capacity && destination[used]) used++;
    while (source && *source && used + 1u < capacity)
        destination[used++] = *source++;
    if (used < capacity) destination[used] = 0;
}

void native_uint_text(unsigned int value, char* out) {
    char reverse[16];
    int count = 0;
    if (!value) {
        out[0] = '0';
        out[1] = 0;
        return;
    }
    while (value && count < 16) {
        reverse[count++] = (char)('0' + value % 10u);
        value /= 10u;
    }
    for (int i = 0; i < count; i++) out[i] = reverse[count - i - 1];
    out[count] = 0;
}

void native_draw_value(gfx_surface_t* surface, int x, int y,
                       unsigned int value) {
    char text[16];
    native_uint_text(value, text);
    g_ui.draw_text(surface, x, y, text, g_ui.text);
}

static int text_compare(const char* left, const char* right) {
    while (*left && *right && *left == *right) {
        left++;
        right++;
    }
    return (unsigned char)*left - (unsigned char)*right;
}

void gui_native_apps_init(const gui_native_ui_t* ui) {
    if (ui) g_ui = *ui;
}

const gui_app_descriptor_t* gui_native_app_descriptor(gui_app_id_t id) {
    if (id == GUI_APP_VIEWER) return gui_viewer_app_descriptor();
    if (id == GUI_APP_TASKS) return gui_tasks_app_descriptor();
    if (id == GUI_APP_NETWORK) return gui_network_app_descriptor();
    return 0;
}

void gui_native_network_pref_set(const char* key, const char* value) {
    if (!key) return;
    if (!text_compare(key, "network_address"))
        native_copy_text(g_pref_address, value, sizeof(g_pref_address));
    else if (!text_compare(key, "network_prefix"))
        native_copy_text(g_pref_prefix, value, sizeof(g_pref_prefix));
    else if (!text_compare(key, "network_gateway"))
        native_copy_text(g_pref_gateway, value, sizeof(g_pref_gateway));
    else if (!text_compare(key, "network_dns"))
        native_copy_text(g_pref_dns, value, sizeof(g_pref_dns));
}

const char* gui_native_network_pref_get(const char* key) {
    if (!key) return "";
    if (!text_compare(key, "network_address")) return g_pref_address;
    if (!text_compare(key, "network_prefix")) return g_pref_prefix;
    if (!text_compare(key, "network_gateway")) return g_pref_gateway;
    if (!text_compare(key, "network_dns")) return g_pref_dns;
    return "";
}
