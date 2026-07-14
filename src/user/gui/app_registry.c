#include "framework.h"

#define GUI_APP_REGISTRY_CAPACITY 16u

static const gui_app_descriptor_t* g_descriptors[GUI_APP_REGISTRY_CAPACITY];
static unsigned int g_descriptor_count;

void gui_app_registry_reset(void) {
    g_descriptor_count = 0;
}

int gui_app_registry_add(const gui_app_descriptor_t* descriptor) {
    unsigned int insert;
    if (!descriptor || descriptor->id == 0 || !descriptor->title ||
        g_descriptor_count >= GUI_APP_REGISTRY_CAPACITY ||
        gui_app_registry_find(descriptor->id)) return 0;
    insert = g_descriptor_count;
    while (insert > 0 &&
           g_descriptors[insert - 1]->launcher_order > descriptor->launcher_order) {
        g_descriptors[insert] = g_descriptors[insert - 1];
        insert--;
    }
    g_descriptors[insert] = descriptor;
    g_descriptor_count++;
    return 1;
}

const gui_app_descriptor_t* gui_app_registry_find(gui_app_id_t id) {
    for (unsigned int i = 0; i < g_descriptor_count; i++)
        if (g_descriptors[i]->id == id) return g_descriptors[i];
    return 0;
}

unsigned int gui_app_registry_count(void) { return g_descriptor_count; }

const gui_app_descriptor_t* gui_app_registry_at(unsigned int index) {
    return index < g_descriptor_count ? g_descriptors[index] : 0;
}

unsigned int gui_app_registry_launcher_count(void) {
    unsigned int count = 0;
    for (unsigned int i = 0; i < g_descriptor_count; i++)
        if (g_descriptors[i]->show_in_start) count++;
    return count;
}

const gui_app_descriptor_t* gui_app_registry_launcher_at(unsigned int index) {
    for (unsigned int i = 0; i < g_descriptor_count; i++) {
        if (!g_descriptors[i]->show_in_start) continue;
        if (!index--) return g_descriptors[i];
    }
    return 0;
}
