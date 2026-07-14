#ifndef SMALLOS_GUI_COMPOSITOR_H
#define SMALLOS_GUI_COMPOSITOR_H

#include <stdint.h>
#include "damage.h"

typedef struct gui_compositor_metrics {
    unsigned int shown_loops, shown_presents, shown_input_events;
    unsigned int shown_max_loop_gap, shown_max_present_ticks;
    unsigned int shown_composed_pixels, shown_presented_pixels;
    unsigned int shown_dirty_regions, shown_full_repaints;
    unsigned int shown_idle_wakeups, shown_pty_wakeups;
    unsigned int total_loops, total_presents, total_input_events;
    unsigned int total_composed_pixels, total_presented_pixels;
    unsigned int total_dirty_regions, total_full_repaints;
    unsigned int total_idle_wakeups, total_pty_wakeups;
} gui_compositor_metrics_t;

void gui_compositor_init(int width, int height, uint32_t now);
void gui_compositor_resize(int width, int height);
void gui_compositor_invalidate(gui_rect_t rect);
void gui_compositor_invalidate_full(void);
const gui_damage_t* gui_compositor_damage(void);
void gui_compositor_clear_damage(void);

int gui_compositor_perf_tick(uint32_t now);
void gui_compositor_note_input(unsigned int count);
void gui_compositor_note_present(uint32_t elapsed_ticks);
void gui_compositor_note_idle_resume(uint32_t now);
void gui_compositor_note_fd_wakeup(void);
void gui_compositor_add_composed(unsigned int pixels);
void gui_compositor_add_presented(unsigned int pixels);
void gui_compositor_note_dirty_region(void);
const gui_compositor_metrics_t* gui_compositor_metrics(void);

void gui_compositor_request_frame(uint32_t now, int immediate);
int gui_compositor_frame_pending(void);
int gui_compositor_frame_due(uint32_t now);
uint32_t gui_compositor_frame_deadline(void);
void gui_compositor_finish_frame(uint32_t now);
void gui_compositor_cancel_frame(void);
void gui_compositor_set_drag_overlay(int visible, int x, int y);
int gui_compositor_drag_overlay(int* x, int* y);

#endif
