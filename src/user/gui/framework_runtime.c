#include "framework_runtime.h"

#include "app_services.h"
#include "builtin_registry.h"
#include "client_surface.h"
#include "framework_internal.h"
#include "modal_manager.h"
#include "poll.h"
#include "theme.h"
#include "user_lib.h"
#include "window_manager.h"

#define TITLE_H 18
#define TASKBAR_H 24

static int g_width;
static int g_height;
static gui_framework_services_t g_services;

static int due(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline) >= 0;
}

static void invalidate_window(gui_window_t* window) {
    if (!window || !gui_wm_active(window) || gui_wm_minimized(window) ||
        !g_services.invalidate_rect) return;
    g_services.invalidate_rect(gui_rect_make(gui_wm_x(window), gui_wm_y(window),
        gui_wm_width(window) + 3, gui_wm_height(window) + 3));
}

static gui_app_context_t context_for(gui_window_t* window) {
    gui_app_context_t context;
    context.window = window;
    context.state = gui_wm_state(window);
    return context;
}

void gui_framework_runtime_init(int width, int height,
                                const gui_framework_services_t* services) {
    g_width = width; g_height = height;
    memset(&g_services, 0, sizeof(g_services));
    if (services) g_services = *services;
    gui_app_registry_reset();
    for (unsigned int i = 0; i < gui_builtin_descriptor_count(); i++)
        (void)gui_app_registry_add(gui_builtin_descriptor_at(i));
}

void gui_framework_runtime_resize(int width, int height) {
    g_width = width; g_height = height;
}

const gui_app_descriptor_t* gui_framework_descriptor(gui_app_id_t id) {
    return gui_app_registry_find(id);
}

unsigned int gui_framework_dispatch(gui_window_t* window,
                                    const gui_app_event_t* event) {
    const gui_app_descriptor_t* descriptor;
    gui_app_context_t context;
    if (!window || !gui_wm_active(window)) return GUI_APP_RESULT_NONE;
    descriptor = gui_framework_descriptor(gui_wm_app_id(window));
    if (!descriptor || !descriptor->event) return GUI_APP_RESULT_NONE;
    context = context_for(window);
    return descriptor->event(&context, event);
}

void gui_framework_close_window(gui_window_t* window) {
    const gui_app_descriptor_t* descriptor;
    gui_app_context_t context;
    if (!window || !gui_wm_active(window)) return;
    gui_modal_dismiss_owner(window);
    descriptor = gui_framework_descriptor(gui_wm_app_id(window));
    context = context_for(window);
    if (descriptor && descriptor->close) descriptor->close(&context);
    if (gui_wm_state(window)) free(gui_wm_state(window));
    gui_wm_set_state(window, 0);
    gui_wm_remove(window);
}

int gui_framework_request_close(gui_window_t* window) {
    gui_app_event_t event;
    unsigned int result;
    if (!window || !gui_wm_active(window)) return 1;
    memset(&event, 0, sizeof(event));
    event.type = GUI_APP_EVENT_CLOSE_REQUEST;
    event.width = gui_wm_width(window);
    event.height = gui_wm_height(window) - TITLE_H;
    event.ticks = sys_get_ticks();
    result = gui_framework_dispatch(window, &event);
    if (result & GUI_APP_RESULT_REDRAW) invalidate_window(window);
    if (result & GUI_APP_RESULT_KEEP_OPEN) return 0;
    invalidate_window(window);
    gui_framework_close_window(window);
    if (g_services.invalidate_taskbar) g_services.invalidate_taskbar();
    return 1;
}

unsigned int gui_framework_apply_result(gui_window_t* window,
                                        unsigned int result) {
    if ((result & GUI_APP_RESULT_REDRAW) && window && gui_wm_active(window))
        invalidate_window(window);
    if ((result & GUI_APP_RESULT_CLOSE) && window && gui_wm_active(window))
        (void)gui_framework_request_close(window);
    return result;
}

static gui_window_t* build_window(gui_app_id_t id, const char* argument) {
    const gui_app_descriptor_t* descriptor = gui_framework_descriptor(id);
    gui_window_t* window;
    int width, height, x, y;
    if (!descriptor) return 0;
    window = gui_wm_allocate();
    if (!window) {
        if (g_services.notice)
            g_services.notice("Window limit reached",
                sys_get_ticks() + SMALLOS_TIMER_HZ * 2u);
        return 0;
    }
    width = descriptor->default_width < descriptor->min_width
          ? descriptor->min_width : descriptor->default_width;
    height = descriptor->default_height < descriptor->min_height
           ? descriptor->min_height : descriptor->default_height;
    x = (g_width - width) / 2; y = (g_height - TASKBAR_H - height) / 2;
    if (x < 4) x = 4; if (y < 20) y = 20;
    if (x > g_width - 32) x = g_width - 32;
    if (y > g_height - TASKBAR_H - TITLE_H)
        y = g_height - TASKBAR_H - TITLE_H;
    gui_wm_set_app_id(window, id);
    gui_wm_set_title(window, descriptor->title);
    gui_wm_set_geometry(window, x, y, width, height);
    gui_wm_set_restore_geometry(window, x, y, width, height);
    gui_wm_set_deadline(window, descriptor->tick_interval
        ? sys_get_ticks() + descriptor->tick_interval : 0u);
    if (descriptor->state_size) {
        gui_wm_set_state(window, malloc(descriptor->state_size));
        if (gui_wm_state(window))
            memset(gui_wm_state(window), 0, descriptor->state_size);
    }
    if (descriptor->state_size && !gui_wm_state(window)) {
        gui_wm_remove(window); return 0;
    }
    if (descriptor->open) {
        gui_app_context_t context = context_for(window);
        descriptor->open(&context, argument);
    }
    return window;
}

gui_window_t* gui_open_app(gui_app_id_t id, const char* argument) {
    gui_window_t* window;
    if (g_width <= 0 || g_height <= 0) return 0;
    window = build_window(id, argument);
    if (window) {
        invalidate_window(window);
        if (g_services.invalidate_taskbar) g_services.invalidate_taskbar();
    }
    return window;
}

void gui_window_request_close(gui_window_t* window) {
    (void)gui_framework_request_close(window);
}

void gui_window_set_title(gui_window_t* window, const char* title) {
    if (!window) return;
    gui_wm_set_title(window, title);
    invalidate_window(window);
    if (g_services.invalidate_taskbar) g_services.invalidate_taskbar();
}

void gui_window_invalidate_local(gui_window_t* window,
                                 int x, int y, int width, int height) {
    if (!window || !gui_wm_active(window) || gui_wm_minimized(window) ||
        width <= 0 || height <= 0 || !g_services.invalidate_rect) return;
    g_services.invalidate_rect(gui_rect_make(gui_wm_x(window) + x,
        gui_wm_y(window) + TITLE_H + y, width, height));
}

void* gui_app_state(gui_app_context_t* context) { return context ? context->state : 0; }
gui_window_t* gui_app_window(gui_app_context_t* context) { return context ? context->window : 0; }
void gui_app_client_size(gui_app_context_t* context, int* width, int* height) {
    if (width) *width = context && context->window ? gui_wm_width(context->window) : 0;
    if (height) *height = context && context->window ? gui_wm_height(context->window) - TITLE_H : 0;
}
int gui_app_focused_control(gui_app_context_t* context) { return context && context->window ? gui_wm_focused_control(context->window) : 0; }
void gui_app_focus_control(gui_app_context_t* context, int id) { if (context && context->window) gui_wm_set_focused_control(context->window, id); }
int gui_app_captured_control(gui_app_context_t* context) { return context && context->window ? gui_wm_captured_control(context->window) : 0; }
void gui_app_capture_pointer(gui_app_context_t* context, int id) { if (context && context->window) gui_wm_set_captured_control(context->window, id); }
void gui_app_release_pointer(gui_app_context_t* context) { if (context && context->window) gui_wm_set_captured_control(context->window, 0); }
void gui_app_set_title(gui_app_context_t* context, const char* title) { if (context) gui_window_set_title(context->window, title); }
void gui_app_invalidate(gui_app_context_t* context, int x, int y, int w, int h) { if (context) gui_window_invalidate_local(context->window, x, y, w, h); }
void gui_app_request_close(gui_app_context_t* context) { if (context) gui_window_request_close(context->window); }
gui_window_t* gui_app_open(gui_app_context_t* context, gui_app_id_t id, const char* argument) { (void)context; return gui_open_app(id, argument); }
int gui_app_open_file_picker(gui_app_context_t* context, gui_file_request_mode_t mode, gui_file_filter_t filter, const char* path) { return context && context->window ? gui_modal_open_picker(context->window, mode, filter, path) : 0; }
int gui_app_open_dialog(gui_app_context_t* context, const gui_dialog_request_t* request) { return context && context->window ? gui_modal_open_dialog(context->window, request) : 0; }

void gui_framework_sync_focus(void) {
    int previous = gui_wm_focused_window();
    gui_window_t* top = gui_wm_top();
    int next = top ? gui_wm_index(top) : -1;
    gui_app_event_t event;
    if (previous == next) return;
    memset(&event, 0, sizeof(event)); event.ticks = sys_get_ticks();
    if (previous >= 0 && gui_wm_active(gui_wm_at(previous))) {
        gui_window_t* old = gui_wm_at(previous);
        event.type = GUI_APP_EVENT_FOCUS_LOST;
        event.width = gui_wm_width(old); event.height = gui_wm_height(old) - TITLE_H;
        gui_framework_apply_result(old, gui_framework_dispatch(old, &event));
    }
    gui_wm_set_focused_window(next);
    if (top) {
        event.type = GUI_APP_EVENT_FOCUS_GAINED;
        event.width = gui_wm_width(top); event.height = gui_wm_height(top) - TITLE_H;
        gui_framework_apply_result(top, gui_framework_dispatch(top, &event));
    }
}

int gui_framework_dispatch_ticks(uint32_t now) {
    int dirty = 0;
    for (int i = 0; i < GUI_WINDOW_CAPACITY; i++) {
        gui_window_t* window = gui_wm_at(i);
        const gui_app_descriptor_t* descriptor;
        gui_app_event_t event; unsigned int result;
        if (!gui_wm_active(window)) continue;
        descriptor = gui_framework_descriptor(gui_wm_app_id(window));
        if (!descriptor || !descriptor->tick_interval ||
            (gui_wm_minimized(window) && !descriptor->background_ticks)) continue;
        if (!gui_wm_deadline(window)) gui_wm_set_deadline(window, now + descriptor->tick_interval);
        if (!due(now, gui_wm_deadline(window))) continue;
        memset(&event, 0, sizeof(event)); event.type = GUI_APP_EVENT_TICK;
        event.width = gui_wm_width(window); event.height = gui_wm_height(window) - TITLE_H; event.ticks = now;
        result = gui_framework_dispatch(window, &event);
        gui_framework_apply_result(window, result);
        if ((result & GUI_APP_RESULT_REDRAW) && !gui_wm_minimized(window)) dirty = 1;
        if (gui_wm_active(window)) gui_wm_set_deadline(window, now + descriptor->tick_interval);
    }
    return dirty;
}

unsigned int gui_framework_poll_fds(struct pollfd* fds, gui_window_t** owners, unsigned int capacity) {
    unsigned int count = 0;
    for (int i = 0; i < GUI_WINDOW_CAPACITY && count < capacity; i++) {
        gui_window_t* window = gui_wm_at(i); const gui_app_descriptor_t* descriptor; gui_app_context_t context; int fd;
        if (!gui_wm_active(window) || !gui_wm_state(window)) continue;
        descriptor = gui_framework_descriptor(gui_wm_app_id(window)); if (!descriptor || !descriptor->wait_fd) continue;
        context = context_for(window); fd = descriptor->wait_fd(&context); if (fd < 0) continue;
        fds[count] = (struct pollfd){fd, POLLIN | POLLHUP | POLLERR, 0}; if (owners) owners[count] = window; count++;
    }
    return count;
}

int gui_framework_dispatch_ready_fds(void) {
    struct pollfd fds[GUI_WINDOW_CAPACITY]; gui_window_t* owners[GUI_WINDOW_CAPACITY];
    unsigned int count = gui_framework_poll_fds(fds, owners, GUI_WINDOW_CAPACITY); int dirty = 0;
    if (!count || poll(fds, count, 0) <= 0) return 0;
    for (unsigned int i = 0; i < count; i++) if (fds[i].revents && gui_wm_active(owners[i])) {
        gui_app_event_t event; unsigned int result; memset(&event, 0, sizeof(event));
        event.type = GUI_APP_EVENT_FD_READY; event.fd = fds[i].fd; event.fd_events = fds[i].revents;
        event.width = gui_wm_width(owners[i]); event.height = gui_wm_height(owners[i]) - TITLE_H; event.ticks = sys_get_ticks();
        result = gui_framework_dispatch(owners[i], &event);
        if (!gui_wm_minimized(owners[i])) { gui_framework_apply_result(owners[i], result); if (result & GUI_APP_RESULT_REDRAW) dirty = 1; }
    }
    return dirty;
}

uint32_t gui_framework_deadline(uint32_t now) {
    uint32_t deadline = 0;
    for (int i = 0; i < GUI_WINDOW_CAPACITY; i++) {
        gui_window_t* window = gui_wm_at(i); const gui_app_descriptor_t* descriptor;
        if (!gui_wm_active(window)) continue; descriptor = gui_framework_descriptor(gui_wm_app_id(window));
        if (!descriptor || !descriptor->tick_interval || (gui_wm_minimized(window) && !descriptor->background_ticks)) continue;
        if (!gui_wm_deadline(window) || due(now, gui_wm_deadline(window))) return now;
        if (!deadline || due(deadline, gui_wm_deadline(window))) deadline = gui_wm_deadline(window);
    }
    return deadline;
}

void gui_framework_close_all(void) {
    for (int i = GUI_WINDOW_CAPACITY - 1; i >= 0; i--)
        if (gui_wm_active(gui_wm_at(i))) gui_framework_close_window(gui_wm_at(i));
}

void gui_framework_draw_client(gui_window_t* window, gfx_surface_t* desktop,
                               gui_rect_t client_screen,
                               const gui_rect_t* screen_clip,
                               int pointer_x, int pointer_y) {
    const gui_app_descriptor_t* descriptor;
    gui_app_context_t context;
    gfx_surface_t client;
    gui_rect_t local_clip;
    int local_x, local_y;
    if (!window || !desktop) return;
    descriptor = gui_framework_descriptor(gui_wm_app_id(window));
    if (!descriptor || !descriptor->draw ||
        !gui_client_surface_prepare(desktop, client_screen, screen_clip,
                                    &client, &local_clip)) return;
    context = context_for(window);
    gui_canvas_set_clip(local_clip);
    gui_client_pointer(client_screen, pointer_x, pointer_y, &local_x, &local_y);
    descriptor->draw(&client, &context, local_x, local_y);
}
