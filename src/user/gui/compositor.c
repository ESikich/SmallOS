#include "compositor.h"

#include "smallos_input.h"
#include "string.h"
#include "time.h"

#define PERF_WINDOW_TICKS SMALLOS_TIMER_HZ
#define FRAME_FPS 60u
#define FRAME_TICKS ((SMALLOS_TIMER_HZ + FRAME_FPS - 1u) / FRAME_FPS)

typedef struct compositor_state {
    int width, height;
    gui_damage_t damage;
    uint32_t window_start, last_loop;
    unsigned int loops, presents, input_events;
    unsigned int max_loop_gap, max_present_ticks;
    unsigned int composed_pixels, presented_pixels;
    unsigned int dirty_regions, full_repaints;
    unsigned int idle_wakeups, pty_wakeups;
    gui_compositor_metrics_t metrics;
    int frame_pending;
    uint32_t frame_deadline;
    int overlay_visible, overlay_x, overlay_y;
} compositor_state_t;

static compositor_state_t g_compositor;

void gui_compositor_init(int width, int height, uint32_t now) {
    memset(&g_compositor, 0, sizeof(g_compositor));
    g_compositor.width = width; g_compositor.height = height;
    g_compositor.window_start = g_compositor.last_loop = now;
}

void gui_compositor_resize(int width, int height) {
    g_compositor.width = width; g_compositor.height = height;
}

void gui_compositor_invalidate(gui_rect_t rect) {
    gui_damage_add(&g_compositor.damage, rect,
                   g_compositor.width, g_compositor.height);
}

void gui_compositor_invalidate_full(void) {
    gui_damage_full(&g_compositor.damage,
                    g_compositor.width, g_compositor.height);
    g_compositor.full_repaints++;
    g_compositor.metrics.total_full_repaints++;
}

const gui_damage_t* gui_compositor_damage(void) { return &g_compositor.damage; }
void gui_compositor_clear_damage(void) { gui_damage_clear(&g_compositor.damage); }

int gui_compositor_perf_tick(uint32_t now) {
    unsigned int gap = now - g_compositor.last_loop;
    gui_compositor_metrics_t* m = &g_compositor.metrics;
    g_compositor.last_loop = now; g_compositor.loops++; m->total_loops++;
    if (gap > g_compositor.max_loop_gap) g_compositor.max_loop_gap = gap;
    if ((uint32_t)(now - g_compositor.window_start) < PERF_WINDOW_TICKS)
        return 0;
    m->shown_loops = g_compositor.loops;
    m->shown_presents = g_compositor.presents;
    m->shown_input_events = g_compositor.input_events;
    m->shown_max_loop_gap = g_compositor.max_loop_gap;
    m->shown_max_present_ticks = g_compositor.max_present_ticks;
    m->shown_composed_pixels = g_compositor.composed_pixels;
    m->shown_presented_pixels = g_compositor.presented_pixels;
    m->shown_dirty_regions = g_compositor.dirty_regions;
    m->shown_full_repaints = g_compositor.full_repaints;
    m->shown_idle_wakeups = g_compositor.idle_wakeups;
    m->shown_pty_wakeups = g_compositor.pty_wakeups;
    g_compositor.window_start = now;
    g_compositor.loops = g_compositor.presents = g_compositor.input_events = 0;
    g_compositor.max_loop_gap = g_compositor.max_present_ticks = 0;
    g_compositor.composed_pixels = g_compositor.presented_pixels = 0;
    g_compositor.dirty_regions = g_compositor.full_repaints = 0;
    g_compositor.idle_wakeups = g_compositor.pty_wakeups = 0;
    return 1;
}

void gui_compositor_note_input(unsigned int count) {
    g_compositor.input_events += count;
    g_compositor.metrics.total_input_events += count;
}

void gui_compositor_note_present(uint32_t elapsed) {
    g_compositor.presents++; g_compositor.metrics.total_presents++;
    if (elapsed > g_compositor.max_present_ticks)
        g_compositor.max_present_ticks = elapsed;
}

void gui_compositor_note_idle_resume(uint32_t now) {
    g_compositor.last_loop = now; g_compositor.idle_wakeups++;
    g_compositor.metrics.total_idle_wakeups++;
}

void gui_compositor_note_fd_wakeup(void) {
    g_compositor.pty_wakeups++;
    g_compositor.metrics.total_pty_wakeups++;
}

void gui_compositor_add_composed(unsigned int pixels) {
    g_compositor.composed_pixels += pixels;
    g_compositor.metrics.total_composed_pixels += pixels;
}

void gui_compositor_add_presented(unsigned int pixels) {
    g_compositor.presented_pixels += pixels;
    g_compositor.metrics.total_presented_pixels += pixels;
}

void gui_compositor_note_dirty_region(void) {
    g_compositor.dirty_regions++;
    g_compositor.metrics.total_dirty_regions++;
}

const gui_compositor_metrics_t* gui_compositor_metrics(void) {
    return &g_compositor.metrics;
}

void gui_compositor_request_frame(uint32_t now, int immediate) {
    g_compositor.frame_pending = 1;
    if (immediate || !g_compositor.frame_deadline ||
        (int32_t)(now - g_compositor.frame_deadline) >= 0)
        g_compositor.frame_deadline = now;
}
int gui_compositor_frame_pending(void) { return g_compositor.frame_pending; }
int gui_compositor_frame_due(uint32_t now) { return g_compositor.frame_pending && (int32_t)(now - g_compositor.frame_deadline) >= 0; }
uint32_t gui_compositor_frame_deadline(void) { return g_compositor.frame_deadline; }
void gui_compositor_finish_frame(uint32_t now) { g_compositor.frame_pending = 0; g_compositor.frame_deadline = now + FRAME_TICKS; }
void gui_compositor_cancel_frame(void) { g_compositor.frame_pending = 0; g_compositor.frame_deadline = 0; }
void gui_compositor_set_drag_overlay(int visible, int x, int y) { g_compositor.overlay_visible = visible; g_compositor.overlay_x = x; g_compositor.overlay_y = y; }
int gui_compositor_drag_overlay(int* x, int* y) { if (x) *x = g_compositor.overlay_x; if (y) *y = g_compositor.overlay_y; return g_compositor.overlay_visible; }
