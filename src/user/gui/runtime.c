/*
 * gui — interactive desktop for SmallOS.
 *
 * Real, working pieces (not a screenshot):
 *   - software mouse cursor driven by the kernel input queue
 *   - clickable desktop icons that open application windows
 *   - draggable windows with a working close button
 *   - z-order: clicking a window raises it to the front
 *   - "Files" window: live directory listing via opendir/readdir,
 *     click a row to descend, click ".." to ascend
 *   - "System" window: live sys_fsinfo, sys_display_info, sys_get_ticks, pid
 *   - "Config" window: persistent desktop settings
 *   - "About" window: built-in static info (no fake screens)
 *   - "Quit" icon exits and releases the display
 *
 * ESC also exits.
 */

#include "user_lib.h"
#include "gfx.h"
#include "smallos_input.h"
#include "keyboard.h"
#include "dirent.h"
#include "gui.h"
#include "canvas.h"
#include "client_surface.h"
#include "compositor.h"
#include "app_event.h"
#include "app_services.h"
#include "builtin_registry.h"
#include "cursor.h"
#include "damage.h"
#include "desktop_shell.h"
#include "desktop_model.h"
#include "file_picker.h"
#include "framework.h"
#include "framework_runtime.h"
#include "modal_manager.h"
#include "native_apps.h"
#include "occlusion.h"
#include "preferences.h"
#include "region.h"
#include "runtime.h"
#include "theme.h"
#include "widgets.h"
#include "window.h"
#include "window_manager.h"
#include "time.h"
#include "poll.h"

typedef int gui_fp_t;

#define GUI_WIDGET_THEME gui_retro_widget_theme
#define GUI_BUILTIN_STYLE gui_retro_builtin_style
#define draw_text gui_theme_draw_text
#define draw_fixed_text gui_theme_draw_fixed_text
#define text_width gui_theme_text_width

#define make_rect gui_rect_make
#define rect_empty gui_rect_empty
#define rect_intersects gui_rect_intersects
#define rect_union gui_rect_union
#define rect_should_merge gui_rect_should_merge
#define rect_clip_screen gui_rect_clip
#define clip_set gui_canvas_set_clip
#define clip_clear gui_canvas_clear_clip
#define gui_put_pixel gui_canvas_put_pixel
#define fillr gui_canvas_fill_rect
#define hline gui_canvas_hline
#define vline gui_canvas_vline
#define rect gui_canvas_rect
#define draw_cursor gui_cursor_draw
#define cursor_screen_rect gui_cursor_rect
#define g_clip_enabled gui_canvas_has_clip()
#define g_clip_rect (*gui_canvas_clip())

static unsigned int u_strlen(const char* s) {
    unsigned int n = 0;
    while (s[n]) n++;
    return n;
}

static void u_strcpy_n(char* dst, const char* src, unsigned int cap) {
    unsigned int i = 0;
    while (i + 1 < cap && src[i]) { dst[i] = src[i]; i++; }
    if (cap) dst[i] = 0;
}
static void u_strcat_n(char* dst, const char* src, unsigned int cap) {
    unsigned int n = u_strlen(dst);
    while (n + 1 < cap && *src) { dst[n++] = *src++; }
    if (cap) dst[n] = 0;
}
static int u_streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static void utoa10(unsigned int v, char* buf) {
    char tmp[16]; int n = 0;
    if (v == 0) { buf[0] = '0'; buf[1] = 0; return; }
    while (v > 0 && n < (int)sizeof(tmp)) { tmp[n++] = (char)('0' + v % 10u); v /= 10u; }
    int j = 0;
    while (n > 0) buf[j++] = tmp[--n];
    buf[j] = 0;
}

static unsigned int sample_self_ram(void) {
    sys_procinfo_t info;
    unsigned int pid = (unsigned int)sys_getpid();
    if (sys_procinfo(&info) < 0) return 0;
    for (unsigned int i = 0; i < info.out_count; i++)
        if (info.entries[i].pid == pid) return info.entries[i].ram_bytes;
    return 0;
}

static void diagnostic_append(char* line, unsigned int capacity,
                              const char* name, unsigned int value) {
    char number[16];
    u_strcat_n(line, " ", capacity);
    u_strcat_n(line, name, capacity);
    u_strcat_n(line, "=", capacity);
    utoa10(value, number);
    u_strcat_n(line, number, capacity);
}

/* ---------------- window model ---------------- */

#define MAX_WINDOWS GUI_WINDOW_CAPACITY
#define INPUT_BATCH 32
#define MOUSE_COALESCE_MAX INPUT_BATCH
#define MOUSE_MERGE_MAX_DELTA 48
#define GUI_PERF_WINDOW_TICKS SMALLOS_TIMER_HZ
#define GUI_FP_SHIFT 8
#define GUI_FP_ONE (1 << GUI_FP_SHIFT)
/*
 * The PIT runs at 300 Hz, so 60 FPS frame pacing is exactly 5 ticks.
 * Keep this derived so a later timer-rate change keeps the intent visible.
 */
#define GUI_FRAME_FPS 60u
#define GUI_FRAME_TICKS \
    ((SMALLOS_TIMER_HZ + GUI_FRAME_FPS - 1u) / GUI_FRAME_FPS)

typedef gui_app_id_t win_type_t;

typedef gui_window_t window_t;
typedef gui_app_descriptor_t window_app_ops_t;

static const window_app_ops_t* window_app_ops(win_type_t type);
#define dispatch_app_event(window, event) gui_framework_dispatch((window), (event))
#define apply_app_result(window, result, sw, sh) gui_framework_apply_result((window), (result))
#define request_window_close(window, sw, sh) gui_framework_request_close((window))
#define close_window(window) gui_framework_close_window((window))
#define sync_window_focus(sw, sh) gui_framework_sync_focus()
#define dispatch_due_app_ticks(sw, sh, now) gui_framework_dispatch_ticks((now))
#define dispatch_ready_app_fds(sw, sh) gui_framework_dispatch_ready_fds()
#define app_poll_fds(fds, owners, capacity) gui_framework_poll_fds((fds), (owners), (capacity))

static int g_screen_width;
static int g_screen_height;

static int point_in(int x, int y, int rx, int ry, int rw, int rh);

#define TITLE_H 18
#define CLOSE_W 14
#define TITLE_BUTTON_W 14
#define ROW_H   12
#define RESIZE_GRIP 10
#define TASKBAR_H 24
#define START_MENU_W 174
#define START_MENU_ROW_H 18
#define WINDOW_MIN_W 160
#define WINDOW_MIN_H 80
#define CURSOR_W GUI_CURSOR_WIDTH
#define CURSOR_H GUI_CURSOR_HEIGHT
#define CURSOR_MOVE_MAX_W 64
#define CURSOR_MOVE_MAX_H 64

#define g_dirty (gui_compositor_damage()->rects)
#define g_dirty_count (gui_compositor_damage()->count)
#define g_dirty_full (gui_compositor_damage()->full)

typedef enum {
    DRAG_NONE = 0,
    DRAG_MOVE,
    DRAG_RESIZE,
} drag_mode_t;

static drag_mode_t g_drag = DRAG_NONE;
static int g_drag_idx = -1;
static gui_fp_t g_drag_dx_fp = 0, g_drag_dy_fp = 0;
static int g_drag_preview_x = 0, g_drag_preview_y = 0;
static int g_drag_overlay_visible = 0;
static int g_drag_overlay_x = 0, g_drag_overlay_y = 0;
static int g_resize_start_mx = 0, g_resize_start_my = 0;
static int g_resize_start_w = 0, g_resize_start_h = 0;
static int g_last_title_click_window = -1;
static uint32_t g_last_title_click_tick = 0;

static int g_diagnostics = 0;
static unsigned int g_startup_ram_bytes = 0;

static void diagnostics_print(void) {
    char line[384];
    const gui_compositor_metrics_t* metrics = gui_compositor_metrics();
    u_strcpy_n(line, "gui: metrics", sizeof(line));
    diagnostic_append(line, sizeof(line), "startup_ram", g_startup_ram_bytes);
    diagnostic_append(line, sizeof(line), "saved_presentbuffer",
                      (unsigned int)g_screen_width *
                      (unsigned int)g_screen_height * 4u);
    diagnostic_append(line, sizeof(line), "loops", metrics->total_loops);
    diagnostic_append(line, sizeof(line), "presents", metrics->total_presents);
    diagnostic_append(line, sizeof(line), "input", metrics->total_input_events);
    diagnostic_append(line, sizeof(line), "composed", metrics->total_composed_pixels);
    diagnostic_append(line, sizeof(line), "presented", metrics->total_presented_pixels);
    diagnostic_append(line, sizeof(line), "dirty", metrics->total_dirty_regions);
    diagnostic_append(line, sizeof(line), "full", metrics->total_full_repaints);
    diagnostic_append(line, sizeof(line), "idle", metrics->total_idle_wakeups);
    diagnostic_append(line, sizeof(line), "pty", metrics->total_pty_wakeups);
    u_strcat_n(line, "\n", sizeof(line));
    u_puts(line);
}

static void invalidate_topbar(int sw, int sh);

void gui_app_services_draw_text(gfx_surface_t* surface, int x, int y,
                                const char* text, unsigned int color) {
    draw_text(surface, x, y, text, color);
}

const gui_builtin_style_t* gui_app_services_builtin_style(void) {
    return &GUI_BUILTIN_STYLE;
}

const gui_widget_theme_t* gui_app_services_widget_theme(void) {
    return &GUI_WIDGET_THEME;
}

int gui_app_services_performance_visible(void) {
    return gui_preferences_performance_visible();
}

void gui_app_services_performance_snapshot(gui_app_perf_snapshot_t* snapshot) {
    const gui_compositor_metrics_t* metrics = gui_compositor_metrics();
    if (!snapshot) return;
    snapshot->composed_pixels = metrics->shown_composed_pixels;
    snapshot->presented_pixels = metrics->shown_presented_pixels;
    snapshot->dirty_regions = metrics->shown_dirty_regions;
    snapshot->full_repaints = metrics->shown_full_repaints;
    snapshot->idle_wakeups = metrics->shown_idle_wakeups;
    snapshot->pty_wakeups = metrics->shown_pty_wakeups;
}

void gui_app_services_toggle_performance(void) {
    gui_preferences_toggle_performance();
    invalidate_topbar(g_screen_width, g_screen_height);
}


static void invalidate_rect(int sw, int sh, gui_rect_t r);

static int win_index(window_t* w) { return gui_wm_index(w); }

static void z_remove_idx(int win_index_val) {
    gui_wm_remove(gui_wm_at(win_index_val));
}

static void z_push_top(int win_index_val) {
    gui_wm_raise(gui_wm_at(win_index_val));
}

static window_t* topmost(void) { return gui_wm_top(); }

static window_t* hit_window_z(int mx, int my) { return gui_wm_hit(mx, my); }

static gui_rect_t window_screen_rect(window_t* w) {
    if (!w || !gui_wm_active(w) || gui_wm_minimized(w)) return make_rect(0, 0, 0, 0);
    return make_rect(gui_wm_x(w), gui_wm_y(w), gui_wm_width(w) + 3, gui_wm_height(w) + 3);
}

static void dirty_clear(void) {
    gui_compositor_clear_damage();
}

static void invalidate_full(int sw, int sh) {
    (void)sw; (void)sh;
    gui_compositor_invalidate_full();
}

static void invalidate_rect(int sw, int sh, gui_rect_t r) {
    (void)sw; (void)sh;
    gui_compositor_invalidate(r);
}

static void invalidate_window(int sw, int sh, window_t* w) {
    invalidate_rect(sw, sh, window_screen_rect(w));
}

static void invalidate_topbar(int sw, int sh) {
    invalidate_rect(sw, sh, make_rect(0, sh - TASKBAR_H, sw, TASKBAR_H));
}

static int perf_tick(int sw, int sh) {
    if (gui_compositor_perf_tick(sys_get_ticks())) {
        invalidate_topbar(sw, sh);
        return 1;
    }
    return 0;
}

static void perf_note_input(unsigned int events) {
    gui_compositor_note_input(events);
}

static void perf_note_present(uint32_t start_tick) {
    uint32_t now = sys_get_ticks();
    unsigned int ticks = now - start_tick;

    gui_compositor_note_present(ticks);
}

static void perf_resume_from_idle(void) {
    uint32_t now = sys_get_ticks();
    gui_compositor_note_idle_resume(now);
}

static gui_rect_t drag_preview_rect(void) {
    if (g_drag != DRAG_MOVE || g_drag_idx < 0 || g_drag_idx >= MAX_WINDOWS ||
        !gui_wm_active(gui_wm_at(g_drag_idx))) {
        return make_rect(0, 0, 0, 0);
    }
    return make_rect(g_drag_preview_x, g_drag_preview_y,
                     gui_wm_width(gui_wm_at(g_drag_idx)), gui_wm_height(gui_wm_at(g_drag_idx)));
}

static gui_rect_t drag_overlay_rect(void) {
    if (!g_drag_overlay_visible ||
        g_drag != DRAG_MOVE ||
        g_drag_idx < 0 ||
        g_drag_idx >= MAX_WINDOWS ||
        !gui_wm_active(gui_wm_at(g_drag_idx))) {
        return make_rect(0, 0, 0, 0);
    }
    return make_rect(g_drag_overlay_x, g_drag_overlay_y,
                     gui_wm_width(gui_wm_at(g_drag_idx)), gui_wm_height(gui_wm_at(g_drag_idx)));
}

static int drag_target_pending(void) {
    return g_drag == DRAG_MOVE &&
           g_drag_idx >= 0 &&
           g_drag_idx < MAX_WINDOWS &&
           gui_wm_active(gui_wm_at(g_drag_idx)) &&
           (!g_drag_overlay_visible ||
            g_drag_overlay_x != g_drag_preview_x ||
            g_drag_overlay_y != g_drag_preview_y);
}

static void gui_request_frame(uint32_t now) {
    gui_compositor_request_frame(now, 0);
}

static void gui_request_frame_now(uint32_t now) {
    gui_compositor_request_frame(now, 1);
}

static int gui_frame_pending(void) {
    return gui_compositor_frame_pending();
}

static int gui_frame_due(uint32_t now) {
    return gui_compositor_frame_due(now);
}

static uint32_t gui_frame_deadline(void) {
    return gui_compositor_frame_deadline();
}

static void gui_finish_frame(uint32_t now) {
    gui_compositor_finish_frame(now);
}

static void gui_cancel_frame(void) {
    gui_compositor_cancel_frame();
}

static int tick_due(uint32_t now, uint32_t deadline) {
    return (int)(now - deadline) >= 0;
}

static uint32_t min_deadline(uint32_t a, uint32_t b) {
    return tick_due(a, b) ? b : a;
}

/* ---------------- file launcher ---------------- */

typedef enum {
    LAUNCH_NONE = 0,
    LAUNCH_ELF,
    LAUNCH_EDIT,
    LAUNCH_BMP,
} launch_kind_t;

static launch_kind_t g_launch_kind = LAUNCH_NONE;
static char g_launch_path[256];

static int s_ends_with(const char* s, const char* suffix) {
    unsigned int n = u_strlen(s);
    unsigned int m = u_strlen(suffix);
    if (m > n) return 0;
    return u_streq(s + n - m, suffix);
}

static int s_starts_with(const char* s, const char* prefix) {
    while (*prefix) {
        if (*s++ != *prefix++) return 0;
    }
    return 1;
}

static void basename_of(const char* path, char* out, unsigned int cap) {
    const char* base = path;
    const char* p = path;
    while (*p) {
        if (*p == '/') base = p + 1;
        p++;
    }
    u_strcpy_n(out, base, cap);
}

static int is_text_like_file(const char* path) {
    return s_ends_with(path, ".txt") ||
           s_ends_with(path, ".TXT") ||
           s_ends_with(path, ".c") ||
           s_ends_with(path, ".h") ||
           s_ends_with(path, ".md") ||
           s_ends_with(path, ".ini") ||
           s_ends_with(path, ".log") ||
           s_ends_with(path, ".html");
}

static int is_program_path(const char* path) {
    const char* base = path;
    const char* p = path;

    while (*p) {
        if (*p == '/') base = p + 1;
        p++;
    }
    if (!base[0]) return 0;
    for (p = base; *p; p++) {
        if (*p == '.') return 0;
    }

    return s_starts_with(path, "/bin/") ||
           s_starts_with(path, "/usr/bin/") ||
           s_starts_with(path, "/usr/sbin/") ||
           s_starts_with(path, "/usr/libexec/tests/");
}

static launch_kind_t launch_kind_for_path(const char* path) {
    if (s_ends_with(path, ".elf")) return LAUNCH_ELF;
    if (is_program_path(path)) return LAUNCH_ELF;
    if (s_ends_with(path, ".bmp") || s_ends_with(path, ".BMP")) return LAUNCH_BMP;
    if (is_text_like_file(path)) return LAUNCH_EDIT;
    return LAUNCH_NONE;
}

int gui_app_services_open_path(gui_app_context_t* context, const char* path) {
    launch_kind_t kind = launch_kind_for_path(path);
    (void)context;
    if (kind == LAUNCH_EDIT) {
        return gui_app_open(context, GUI_APP_EDITOR, path) ? 1 : 0;
    }
    if (kind == LAUNCH_BMP) {
        return gui_app_open(context, GUI_APP_VIEWER, path) ? 2 : 0;
    }
    if (kind == LAUNCH_ELF) {
        g_launch_kind = kind;
        u_strcpy_n(g_launch_path, path, sizeof(g_launch_path));
        return 3;
    }
    return 0;
}

static int run_queued_launch(gfx_context_t* gfx, int* sw, int* sh) {
    launch_kind_t kind = g_launch_kind;
    char path[256];
    char name[NAME_MAX + 1];
    char* argv[3];
    int pid;
    int status = 0;

    if (kind == LAUNCH_NONE) return 0;

    u_strcpy_n(path, g_launch_path, sizeof(path));
    g_launch_kind = LAUNCH_NONE;
    g_launch_path[0] = 0;

    gfx_close(gfx);

    if (kind == LAUNCH_ELF) {
        basename_of(path, name, sizeof(name));
        argv[0] = name;
        argv[1] = 0;
        pid = sys_exec_foreground(path, 1, argv);
    } else if (kind == LAUNCH_BMP) {
        argv[0] = "bmpview";
        argv[1] = path;
        argv[2] = 0;
        pid = sys_exec_foreground("/bin/bmpview", 2, argv);
    } else {
        argv[0] = "edit";
        argv[1] = path;
        argv[2] = 0;
        pid = sys_exec_foreground("/bin/edit", 2, argv);
    }

    if (pid >= 0) {
        if (sys_waitpid_foreground(pid, &status) < 0) {
            (void)sys_waitpid(pid, &status, 0);
        }
    } else {
        u_puts("gui: launch failed: ");
        u_puts(path);
        u_putc('\n');
        sys_sleep(30);
    }

    if (gfx_open(gfx) < 0) {
        u_puts("gui: could not reacquire display\n");
        return -1;
    }

    *sw = (int)gfx->backbuffer.width;
    *sh = (int)gfx->backbuffer.height;
    g_screen_width = *sw;
    g_screen_height = *sh;
    gui_desktop_shell_resize(*sw, *sh);
    return 1;
}

/* ---------------- drawing windows ---------------- */

static void draw_title_button(gfx_surface_t* s, int x, int y, int kind) {
    fillr(s, x, y, TITLE_BUTTON_W - 2, TITLE_H - 4, COL_WIN_BG);
    rect(s, x, y, TITLE_BUTTON_W - 2, TITLE_H - 4, COL_FRAME);
    if (kind == 0) {
        hline(s, x + 3, y + TITLE_H - 8, 6, COL_FRAME);
    } else if (kind == 1) {
        rect(s, x + 3, y + 3, 6, 6, COL_FRAME);
    } else {
        int ix = x + 3, iy = y + 3;
        for (int i = 0; i < 6; i++) {
            gui_put_pixel(s, ix + i, iy + i, COL_FRAME);
            gui_put_pixel(s, ix + 5 - i, iy + i, COL_FRAME);
        }
    }
}

static void draw_title_bar(gfx_surface_t* s, window_t* w, int focused, const char* title) {
    unsigned int bg = focused ? COL_TITLE_BG : COL_TITLE_IDLE_BG;
    fillr(s, gui_wm_x(w), gui_wm_y(w), gui_wm_width(w), TITLE_H, bg);
    draw_text(s, gui_wm_x(w) + 4, gui_wm_y(w) + 6, title, COL_TITLE_FG);
    int cx = gui_wm_x(w) + gui_wm_width(w) - CLOSE_W - 2;
    int cy = gui_wm_y(w) + 2;
    draw_title_button(s, cx - TITLE_BUTTON_W * 2, cy, 0);
    draw_title_button(s, cx - TITLE_BUTTON_W, cy, 1);
    draw_title_button(s, cx, cy, 2);
}

static const window_app_ops_t* window_app_ops(win_type_t type) {
    return gui_framework_descriptor(type);
}

static void modal_dispatch(gui_window_t* owner,
                           const gui_app_event_t* event, void* opaque) {
    (void)opaque;
    apply_app_result(owner, dispatch_app_event(owner, event),
                     g_screen_width, g_screen_height);
}

static void modal_invalidate(void* opaque) {
    (void)opaque;
    invalidate_full(g_screen_width, g_screen_height);
}

static void modal_notice(const char* text, void* opaque) {
    (void)opaque;
    gui_desktop_shell_set_notice(text ? text : "",
        sys_get_ticks() + SMALLOS_TIMER_HZ * 2u);
}

static int modal_owner_active(gui_window_t* owner, void* opaque) {
    (void)opaque;
    return owner && gui_wm_active(owner);
}

static void draw_window(gfx_surface_t* s, window_t* w, int focused, int mx, int my) {
    const window_app_ops_t* ops = window_app_ops(gui_wm_app_id(w));
    gui_rect_t client_screen;
    gui_rect_t saved_clip = make_rect(0, 0, 0, 0);
    int had_clip = g_clip_enabled;
    /* drop shadow */
    fillr(s, gui_wm_x(w) + 3, gui_wm_y(w) + gui_wm_height(w), gui_wm_width(w), 3, COL_SHADOW);
    fillr(s, gui_wm_x(w) + gui_wm_width(w), gui_wm_y(w) + 3, 3, gui_wm_height(w), COL_SHADOW);

    fillr(s, gui_wm_x(w), gui_wm_y(w), gui_wm_width(w), gui_wm_height(w), COL_WIN_BG);
    draw_title_bar(s, w, focused,
                   gui_wm_title(w)[0] ? gui_wm_title(w) : (ops ? ops->title : ""));
    client_screen = make_rect(gui_wm_x(w), gui_wm_y(w) + TITLE_H,
                              gui_wm_width(w), gui_wm_height(w) - TITLE_H);
    if (had_clip) saved_clip = g_clip_rect;
    gui_framework_draw_client(w, s, client_screen,
                              had_clip ? &saved_clip : 0, mx, my);
    if (had_clip) clip_set(saved_clip); else clip_clear();

    /* Bottom-right resize grip. */
    for (int i = 0; i < 3; i++) {
        int inset = 2 + i * 3;
        hline(s, gui_wm_x(w) + gui_wm_width(w) - inset - 2, gui_wm_y(w) + gui_wm_height(w) - 2,
              inset, COL_SHADOW);
    }
}

static int g_should_quit = 0;

static void action_quit(int sw, int sh) {
    int can_quit = 1;
    for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
        if (gui_wm_active(gui_wm_at(i)) && !request_window_close(gui_wm_at(i), sw, sh))
            can_quit = 0;
    }
    if (can_quit) g_should_quit = 1;
}

/* ---------------- compose ---------------- */

static void compose_v2(gfx_surface_t* s, int mx, int my) {
    gui_compositor_add_composed(s->width * s->height);
    clip_clear();
    gui_desktop_shell_draw_background(s);

    int hi = gui_desktop_shell_icon_hit(mx, my);
    if (hit_window_z(mx, my)) hi = -1;
    gui_desktop_shell_draw_icons(s, hi);

    window_t* top = topmost();
    for (int i = 0; i < gui_wm_stack_count(); i++) {
        window_t* w = gui_wm_stack_at(i);
        if (!gui_wm_active(w) || gui_wm_minimized(w)) continue;
        draw_window(s, w, w == top, mx, my);
    }
    gui_desktop_shell_draw_start(s);
    gui_modal_draw(s);
    gui_desktop_shell_draw_taskbar(s);
}

static int present_cursor_rect(gfx_context_t* gfx, int mx, int my, int draw);

static unsigned int compose_rect(gfx_surface_t* s, gui_rect_t r,
                                 int mx, int my) {
    gui_rect_t opaque[GUI_WINDOW_CAPACITY + 3];
    gui_rect_t visible[GUI_VISIBLE_REGION_CAPACITY];
    int window_count = gui_wm_stack_count();
    unsigned int pixels = 0;
    int opaque_count = 0;
    int visible_count;
    int hi = gui_desktop_shell_icon_hit(mx, my);
    window_t* top = topmost();

    if (hit_window_z(mx, my)) hi = -1;
    if (gui_modal_active()) opaque[opaque_count++] = gui_modal_bounds();
    if (gui_desktop_shell_start_open())
        opaque[opaque_count++] = gui_desktop_shell_start_bounds();
    opaque[opaque_count++] = make_rect(0, (int)s->height - TASKBAR_H,
                                       (int)s->width, TASKBAR_H);
    for (int i = window_count - 1; i >= 0; i--) {
        window_t* w = gui_wm_stack_at(i);
        if (gui_wm_active(w) && !gui_wm_minimized(w))
            opaque[opaque_count++] = make_rect(gui_wm_x(w), gui_wm_y(w), gui_wm_width(w), gui_wm_height(w));
    }
    visible_count = gui_visible_regions(r, opaque, opaque_count,
                                        visible, GUI_VISIBLE_REGION_CAPACITY);
    if (visible_count < 0) visible_count = 1, visible[0] = r;
    for (int p = 0; p < visible_count; p++) {
        clip_set(visible[p]);
        gui_desktop_shell_draw_background(s);
        gui_desktop_shell_draw_icons(s, hi);
        pixels += (unsigned int)visible[p].w * (unsigned int)visible[p].h;
    }

    for (int i = 0; i < window_count; i++) {
        window_t* w = gui_wm_stack_at(i);
        opaque_count = 0;
        if (!gui_wm_active(w) || gui_wm_minimized(w) ||
            !rect_intersects(r, window_screen_rect(w))) continue;
        if (gui_modal_active()) opaque[opaque_count++] = gui_modal_bounds();
        if (gui_desktop_shell_start_open())
            opaque[opaque_count++] = gui_desktop_shell_start_bounds();
        opaque[opaque_count++] = make_rect(0, (int)s->height - TASKBAR_H,
                                           (int)s->width, TASKBAR_H);
        for (int above = window_count - 1; above > i; above--) {
            window_t* covering = gui_wm_stack_at(above);
            if (gui_wm_active(covering) && !gui_wm_minimized(covering))
                opaque[opaque_count++] = make_rect(gui_wm_x(covering), gui_wm_y(covering),
                                                   gui_wm_width(covering), gui_wm_height(covering));
        }
        visible_count = gui_visible_regions(r, opaque, opaque_count,
                                            visible, GUI_VISIBLE_REGION_CAPACITY);
        if (visible_count < 0) visible_count = 1, visible[0] = r;
        for (int p = 0; p < visible_count; p++) {
            if (!rect_intersects(visible[p], window_screen_rect(w))) continue;
            clip_set(visible[p]);
            draw_window(s, w, w == top, mx, my);
            pixels += (unsigned int)visible[p].w * (unsigned int)visible[p].h;
        }
    }
    clip_set(r);
    if (gui_desktop_shell_start_open() &&
        rect_intersects(r, gui_desktop_shell_start_bounds())) {
        gui_desktop_shell_draw_start(s);
        pixels += (unsigned int)r.w * (unsigned int)r.h;
    }
    if (gui_modal_active() && rect_intersects(r, gui_modal_bounds())) {
        gui_modal_draw(s);
        pixels += (unsigned int)r.w * (unsigned int)r.h;
    }
    if (rect_intersects(r, make_rect(0, (int)s->height - TASKBAR_H,
                                     (int)s->width, TASKBAR_H))) {
        gui_desktop_shell_draw_taskbar(s);
        pixels += (unsigned int)r.w * (unsigned int)r.h;
    }
    clip_clear();
    return pixels;
}

static int present_dirty_scene(gfx_context_t* gfx,
                               int mx,
                               int my,
                               int cursor_mx,
                               int cursor_my) {
    gui_rect_t cursor = cursor_screen_rect(cursor_mx, cursor_my);
    int sw = (int)gfx->backbuffer.width;
    int sh = (int)gfx->backbuffer.height;
    int cursor_touched = 0;

    for (int i = 0; i < g_dirty_count; i++) {
        gui_rect_t r = rect_clip_screen(g_dirty[i], sw, sh);
        gui_rect_t pieces[4];
        int piece_count;
        if (rect_empty(r)) continue;
        unsigned int composed;
        gui_compositor_note_dirty_region();
        composed = compose_rect(&gfx->backbuffer, r, mx, my);
        gui_compositor_add_composed(composed);

        cursor_touched |= rect_intersects(r, cursor);
        piece_count = gui_rect_exclude(r, cursor, pieces);
        for (int piece = 0; piece < piece_count; piece++) {
            gui_rect_t p = pieces[piece];
            uint32_t present_start = sys_get_ticks();
            if (gfx_present_rect(gfx, (unsigned int)p.x, (unsigned int)p.y,
                                 (unsigned int)p.w, (unsigned int)p.h) < 0) {
                return -1;
            }
            perf_note_present(present_start);
            gui_compositor_add_presented((unsigned int)p.w *
                                         (unsigned int)p.h);
        }
    }
    if (cursor_touched) {
        if (present_cursor_rect(gfx, cursor_mx, cursor_my, 1) < 0) {
            return -1;
        }
    }
    dirty_clear();
    return 0;
}

static int present_cursor_rect(gfx_context_t* gfx, int mx, int my, int draw) {
    unsigned int tmp[CURSOR_W * CURSOR_H];
    gfx_surface_t out;
    gfx_surface_t* scene;
    int w = CURSOR_W;
    int h = CURSOR_H;

    if (!gfx) return -1;
    scene = &gfx->backbuffer;
    if (!scene || !scene->pixels || mx < 0 || my < 0 ||
        mx >= (int)scene->width || my >= (int)scene->height) {
        return 0;
    }

    if (mx + w > (int)scene->width) w = (int)scene->width - mx;
    if (my + h > (int)scene->height) h = (int)scene->height - my;
    if (w <= 0 || h <= 0) return 0;

    for (int y = 0; y < h; y++) {
        unsigned int* src = scene->pixels + (my + y) * scene->pitch_pixels + mx;
        for (int x = 0; x < w; x++) {
            tmp[y * w + x] = src[x];
        }
    }

    out.width = (unsigned int)w;
    out.height = (unsigned int)h;
    out.pitch_pixels = (unsigned int)w;
    out.pixels = tmp;
    if (draw) {
        draw_cursor(&out, 0, 0);
    }

    {
        uint32_t present_start = sys_get_ticks();
        int rc = gfx_present_surface(gfx, (unsigned int)mx,
                                     (unsigned int)my, &out);
        if (rc >= 0) perf_note_present(present_start);
        return rc;
    }
}

static int present_frame_with_cursor(gfx_context_t* gfx, int mx, int my) {
    uint32_t present_start = sys_get_ticks();

    if (gfx_present(gfx) < 0) {
        return -1;
    }
    perf_note_present(present_start);
    gui_compositor_add_presented(gfx->backbuffer.width *
                                 gfx->backbuffer.height);
    if (present_cursor_rect(gfx, mx, my, 1) < 0) {
        return -1;
    }
    return 0;
}

static int present_cursor_move(gfx_context_t* gfx,
                               int old_mx,
                               int old_my,
                               int mx,
                               int my) {
    unsigned int tmp[CURSOR_MOVE_MAX_W * CURSOR_MOVE_MAX_H];
    gfx_surface_t out;
    gfx_surface_t* scene;
    int x0;
    int y0;
    int x1;
    int y1;
    int w;
    int h;

    if (old_mx == mx && old_my == my) return 0;
    scene = &gfx->backbuffer;

    x0 = old_mx < mx ? old_mx : mx;
    y0 = old_my < my ? old_my : my;
    x1 = old_mx > mx ? old_mx + CURSOR_W : mx + CURSOR_W;
    y1 = old_my > my ? old_my + CURSOR_H : my + CURSOR_H;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)scene->width) x1 = (int)scene->width;
    if (y1 > (int)scene->height) y1 = (int)scene->height;
    w = x1 - x0;
    h = y1 - y0;

    if (w > 0 && h > 0 &&
        w <= CURSOR_MOVE_MAX_W &&
        h <= CURSOR_MOVE_MAX_H) {
        for (int y = 0; y < h; y++) {
            unsigned int* src = scene->pixels + (y0 + y) * scene->pitch_pixels + x0;
            for (int x = 0; x < w; x++) {
                tmp[y * w + x] = src[x];
            }
        }

        out.width = (unsigned int)w;
        out.height = (unsigned int)h;
        out.pitch_pixels = (unsigned int)w;
        out.pixels = tmp;
        draw_cursor(&out, mx - x0, my - y0);

        {
            uint32_t present_start = sys_get_ticks();
            int rc = gfx_present_surface(gfx, (unsigned int)x0,
                                         (unsigned int)y0, &out);
            if (rc >= 0) perf_note_present(present_start);
            return rc;
        }
    }

    if (present_cursor_rect(gfx, old_mx, old_my, 0) < 0) {
        return -1;
    }
    if (present_cursor_rect(gfx, mx, my, 1) < 0) {
        return -1;
    }
    return 0;
}

static void drag_preview_strips(gui_rect_t r, gui_rect_t strips[4]) {
    strips[0] = make_rect(r.x - 1, r.y - 1, r.w + 2, 3);
    strips[1] = make_rect(r.x - 1, r.y + r.h - 2, r.w + 2, 3);
    strips[2] = make_rect(r.x - 1, r.y, 3, r.h);
    strips[3] = make_rect(r.x + r.w - 2, r.y, 3, r.h);
}

static void draw_drag_outline(gfx_surface_t* s,
                              gui_rect_t surface_rect,
                              gui_rect_t outline_rect) {
    int x = outline_rect.x - surface_rect.x;
    int y = outline_rect.y - surface_rect.y;

    rect(s, x, y, outline_rect.w, outline_rect.h, 0x00FFFFFFu);
    if (outline_rect.w > 2 && outline_rect.h > 2) {
        rect(s, x + 1, y + 1, outline_rect.w - 2, outline_rect.h - 2,
             0x00000000u);
    }
}

static int present_cursor_rect_with_drag_overlay(gfx_context_t* gfx,
                                                 int mx,
                                                 int my,
                                                 int draw) {
    unsigned int tmp[CURSOR_W * CURSOR_H];
    gfx_surface_t out;
    gfx_surface_t* scene;
    gui_rect_t r;
    int w = CURSOR_W;
    int h = CURSOR_H;

    if (!gfx) return -1;
    scene = &gfx->backbuffer;
    if (!scene->pixels || mx < 0 || my < 0 ||
        mx >= (int)scene->width || my >= (int)scene->height) {
        return 0;
    }

    if (mx + w > (int)scene->width) w = (int)scene->width - mx;
    if (my + h > (int)scene->height) h = (int)scene->height - my;
    if (w <= 0 || h <= 0) return 0;
    r = make_rect(mx, my, w, h);

    for (int y = 0; y < h; y++) {
        unsigned int* src = scene->pixels + (my + y) * scene->pitch_pixels + mx;
        for (int x = 0; x < w; x++) {
            tmp[y * w + x] = src[x];
        }
    }

    out.width = (unsigned int)w;
    out.height = (unsigned int)h;
    out.pitch_pixels = (unsigned int)w;
    out.pixels = tmp;
    if (g_drag_overlay_visible) {
        draw_drag_outline(&out, r, drag_overlay_rect());
    }
    if (draw) {
        draw_cursor(&out, 0, 0);
    }

    {
        uint32_t present_start = sys_get_ticks();
        int rc = gfx_present_surface(gfx, (unsigned int)mx,
                                     (unsigned int)my, &out);
        if (rc >= 0) perf_note_present(present_start);
        return rc;
    }
}

static int present_cursor_move_with_drag_overlay(gfx_context_t* gfx,
                                                 int old_mx,
                                                 int old_my,
                                                 int mx,
                                                 int my) {
    unsigned int tmp[CURSOR_MOVE_MAX_W * CURSOR_MOVE_MAX_H];
    gfx_surface_t out;
    gfx_surface_t* scene;
    gui_rect_t r;
    int x0;
    int y0;
    int x1;
    int y1;
    int w;
    int h;

    if (old_mx == mx && old_my == my) return 0;
    if (!gfx) return -1;
    scene = &gfx->backbuffer;
    if (!scene->pixels) return -1;

    x0 = old_mx < mx ? old_mx : mx;
    y0 = old_my < my ? old_my : my;
    x1 = old_mx > mx ? old_mx + CURSOR_W : mx + CURSOR_W;
    y1 = old_my > my ? old_my + CURSOR_H : my + CURSOR_H;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)scene->width) x1 = (int)scene->width;
    if (y1 > (int)scene->height) y1 = (int)scene->height;
    w = x1 - x0;
    h = y1 - y0;

    if (w <= 0 || h <= 0) return 0;
    if (w > CURSOR_MOVE_MAX_W || h > CURSOR_MOVE_MAX_H) {
        if (present_cursor_rect_with_drag_overlay(gfx, old_mx, old_my, 0) < 0) {
            return -1;
        }
        return present_cursor_rect_with_drag_overlay(gfx, mx, my, 1);
    }
    r = make_rect(x0, y0, w, h);

    for (int y = 0; y < h; y++) {
        unsigned int* src = scene->pixels + (y0 + y) * scene->pitch_pixels + x0;
        for (int x = 0; x < w; x++) {
            tmp[y * w + x] = src[x];
        }
    }

    out.width = (unsigned int)w;
    out.height = (unsigned int)h;
    out.pitch_pixels = (unsigned int)w;
    out.pixels = tmp;
    if (g_drag_overlay_visible) {
        draw_drag_outline(&out, r, drag_overlay_rect());
    }
    draw_cursor(&out, mx - x0, my - y0);

    {
        uint32_t present_start = sys_get_ticks();
        int rc = gfx_present_surface(gfx, (unsigned int)x0,
                                     (unsigned int)y0, &out);
        if (rc >= 0) perf_note_present(present_start);
        return rc;
    }
}

static int drag_preview_dirty_add(gui_rect_t* rects,
                                  int* count,
                                  int max_count,
                                  int sw,
                                  int sh,
                                  gui_rect_t r) {
    r = rect_clip_screen(r, sw, sh);
    if (rect_empty(r)) return 0;

    for (int i = 0; i < *count; i++) {
        if (rect_should_merge(rects[i], r)) {
            rects[i] = rect_clip_screen(rect_union(rects[i], r), sw, sh);
            return 0;
        }
    }

    if (*count >= max_count) {
        rects[0] = rect_union(rects[0], r);
        rects[0] = rect_clip_screen(rects[0], sw, sh);
        *count = 1;
        return 0;
    }

    rects[*count] = r;
    *count = *count + 1;
    return 0;
}

static int present_drag_preview_rect(gfx_context_t* gfx,
                                     gui_rect_t r,
                                     gui_rect_t outline,
                                     int draw) {
    unsigned int* tmp;
    gfx_surface_t out;
    int sw;
    int sh;

    if (!gfx || !gfx->backbuffer.pixels) return -1;
    sw = (int)gfx->backbuffer.width;
    sh = (int)gfx->backbuffer.height;
    r = rect_clip_screen(r, sw, sh);
    if (rect_empty(r)) return 0;
    {
        unsigned int needed = (unsigned int)r.w * (unsigned int)r.h;
        unsigned int available = gfx->scratch.width * gfx->scratch.height;
        if (!gfx->scratch.pixels || available < needed) {
            gfx_surface_free(&gfx->scratch);
            if (!gfx_surface_alloc(&gfx->scratch, needed, 1u)) return -1;
        }
    }
    tmp = gfx->scratch.pixels;

    for (int y = 0; y < r.h; y++) {
        unsigned int* src = gfx->backbuffer.pixels +
            (unsigned int)(r.y + y) * gfx->backbuffer.pitch_pixels +
            (unsigned int)r.x;
        for (int x = 0; x < r.w; x++) {
            tmp[y * r.w + x] = src[x];
        }
    }

    out.width = (unsigned int)r.w;
    out.height = (unsigned int)r.h;
    out.pitch_pixels = (unsigned int)r.w;
    out.pixels = tmp;
    if (draw) {
        draw_drag_outline(&out, r, outline);
    }

    {
        uint32_t present_start = sys_get_ticks();
        int rc = gfx_present_surface(gfx, (unsigned int)r.x,
                                     (unsigned int)r.y, &out);
        if (rc >= 0) perf_note_present(present_start);
        return rc;
    }
}

static int present_drag_preview(gfx_context_t* gfx, gui_rect_t r, int draw) {
    gui_rect_t strips[4];

    drag_preview_strips(r, strips);
    for (int i = 0; i < 4; i++) {
        if (present_drag_preview_rect(gfx, strips[i], r, draw) < 0) return -1;
    }
    return 0;
}

static int present_drag_preview_move(gfx_context_t* gfx,
                                     gui_rect_t old_rect,
                                     gui_rect_t new_rect) {
    gui_rect_t strips[4];
    gui_rect_t dirty[8];
    int dirty_count = 0;
    int sw;
    int sh;

    if (!gfx) return -1;
    sw = (int)gfx->backbuffer.width;
    sh = (int)gfx->backbuffer.height;

    drag_preview_strips(old_rect, strips);
    for (int i = 0; i < 4; i++) {
        drag_preview_dirty_add(dirty, &dirty_count, 8, sw, sh, strips[i]);
    }
    drag_preview_strips(new_rect, strips);
    for (int i = 0; i < 4; i++) {
        drag_preview_dirty_add(dirty, &dirty_count, 8, sw, sh, strips[i]);
    }

    for (int i = 0; i < dirty_count; i++) {
        if (present_drag_preview_rect(gfx, dirty[i], new_rect, 1) < 0) {
            return -1;
        }
    }
    return 0;
}

static int present_drag_frame(gfx_context_t* gfx,
                              int mx,
                              int my,
                              uint32_t now) {
    gui_rect_t old_overlay;
    gui_rect_t new_overlay;

    if (g_drag != DRAG_MOVE || g_drag_idx < 0) return 0;

    old_overlay = drag_overlay_rect();
    new_overlay = drag_preview_rect();
    if (g_drag_overlay_visible) {
        if ((old_overlay.x != new_overlay.x || old_overlay.y != new_overlay.y) &&
            present_drag_preview_move(gfx, old_overlay, new_overlay) < 0) {
            return -1;
        }
    } else if (present_drag_preview(gfx, new_overlay, 1) < 0) {
        return -1;
    }

    g_drag_overlay_visible = 1;
    g_drag_overlay_x = g_drag_preview_x;
    g_drag_overlay_y = g_drag_preview_y;

    return present_cursor_rect_with_drag_overlay(gfx, mx, my, 1);
}

static int present_gui_frame(gfx_context_t* gfx,
                             int mx,
                             int my,
                             uint32_t now) {
    int did_work = 0;

    if (drag_target_pending()) {
        if (present_drag_frame(gfx, mx, my, now) < 0) {
            return -1;
        }
        did_work = 1;
    }

    if (did_work) {
        gui_finish_frame(now);
    } else {
        gui_cancel_frame();
    }
    return 0;
}

/* ---------------- input handling ---------------- */

static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int absi(int v) {
    return v < 0 ? -v : v;
}

static gui_fp_t fp_from_int(int v) {
    return (gui_fp_t)(v * GUI_FP_ONE);
}

static int fp_to_int_round(gui_fp_t v) {
    if (v >= 0) return (v + GUI_FP_ONE / 2) >> GUI_FP_SHIFT;
    return -(((-v) + GUI_FP_ONE / 2) >> GUI_FP_SHIFT);
}

static gui_fp_t fp_clamp_int(gui_fp_t v, int lo, int hi) {
    gui_fp_t flo = fp_from_int(lo);
    gui_fp_t fhi = fp_from_int(hi);

    if (v < flo) return flo;
    if (v > fhi) return fhi;
    return v;
}

static gui_fp_t abs_to_screen_fp(unsigned int value, int max_value) {
    unsigned long long scaled;

    if (max_value <= 0) return 0;
    if (value > 65535u) value = 65535u;
    /*
     * Keep absolute-device transforms in subpixel space. Drawing still lands on
     * integer pixels, but cursor/window math should not discard transform
     * precision before the final presentation step.
     */
    scaled = (unsigned long long)value *
             (unsigned long long)(unsigned int)max_value *
             (unsigned long long)GUI_FP_ONE;
    return (gui_fp_t)((scaled + 32767ull) / 65535ull);
}

static int point_in(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static int mouse_event_mergeable(const sys_input_event_t* a,
                                 const sys_input_event_t* b) {
    int dx;
    int dy;

    if (!a || !b) return 0;
    if (a->type != SYS_INPUT_TYPE_MOUSE || b->type != SYS_INPUT_TYPE_MOUSE) return 0;
    if ((a->flags & SYS_INPUT_MOUSE_ABSOLUTE) != (b->flags & SYS_INPUT_MOUSE_ABSOLUTE)) return 0;
    if (a->wheel != 0 || b->wheel != 0) return 0;
    if (a->button_changes != 0u || b->button_changes != 0u) return 0;
    if (a->buttons != b->buttons) return 0;
    dx = a->dx + b->dx;
    dy = a->dy + b->dy;
    if (absi(dx) + absi(dy) > MOUSE_MERGE_MAX_DELTA) return 0;
    return 1;
}

static int read_input_coalesced(sys_input_event_t* events,
                                unsigned int cap,
                                unsigned int flags) {
    sys_input_event_t raw[MOUSE_COALESCE_MAX];
    int total = 0;
    int n;
    int should_coalesce;

    if (!events || cap == 0u) return 0;

    n = sys_input_read(raw, MOUSE_COALESCE_MAX, flags);
    if (n <= 0) return n;
    should_coalesce = n == MOUSE_COALESCE_MAX;

    if (!should_coalesce) {
        for (int i = 0; i < n && i < (int)cap; i++) {
            events[i] = raw[i];
        }
        return n < (int)cap ? n : (int)cap;
    }

    for (int i = 0; i < n && total < (int)cap; i++) {
        sys_input_event_t ev = raw[i];

        while (ev.type == SYS_INPUT_TYPE_MOUSE &&
               ev.button_changes == 0u &&
               ev.wheel == 0 &&
               i + 1 < n &&
               mouse_event_mergeable(&ev, &raw[i + 1])) {
            sys_input_event_t* next = &raw[++i];
            if (ev.flags & SYS_INPUT_MOUSE_ABSOLUTE) {
                ev.abs_x = next->abs_x;
                ev.abs_y = next->abs_y;
                ev.dx += next->dx;
                ev.dy += next->dy;
            } else {
                ev.dx += next->dx;
                ev.dy += next->dy;
            }
            ev.ticks = next->ticks;
            ev.sequence = next->sequence;
        }

        events[total++] = ev;
    }

    return total;
}

static void dispatch_window_resize(window_t* w, uint32_t ticks) {
    gui_app_event_t event;
    if (!w || !gui_wm_active(w)) return;
    memset(&event, 0, sizeof(event));
    event.type = GUI_APP_EVENT_RESIZE;
    event.width = gui_wm_width(w);
    event.height = gui_wm_height(w) - TITLE_H;
    event.ticks = ticks;
    (void)dispatch_app_event(w, &event);
}

static void window_toggle_maximize(window_t* w, int sw, int sh) {
    if (!w || !gui_wm_active(w)) return;
    if (gui_wm_maximized(w)) {
        gui_wm_set_geometry(w, gui_wm_restore_x(w), gui_wm_restore_y(w),
                            gui_wm_restore_w(w), gui_wm_restore_h(w));
        gui_wm_set_maximized(w, 0);
    } else {
        gui_wm_set_restore_geometry(w, gui_wm_x(w), gui_wm_y(w),
                                    gui_wm_width(w), gui_wm_height(w));
        gui_wm_set_geometry(w, 0, 0, sw, sh - TASKBAR_H);
        gui_wm_set_maximized(w, 1);
    }
    gui_wm_set_minimized(w, 0);
    z_push_top(win_index(w));
    dispatch_window_resize(w, sys_get_ticks());
    invalidate_full(sw, sh);
}

static void window_snap(window_t* w, int sw, int sh, int side) {
    if (!w || !gui_wm_active(w)) return;
    if (!gui_wm_maximized(w)) {
        gui_wm_set_restore_geometry(w, gui_wm_x(w), gui_wm_y(w),
                                    gui_wm_width(w), gui_wm_height(w));
    }
    gui_wm_set_maximized(w, 0);
    gui_wm_set_geometry(w, side < 0 ? 0 : sw / 2, 0,
                        side < 0 ? sw / 2 : sw - sw / 2,
                        sh - TASKBAR_H);
    dispatch_window_resize(w, sys_get_ticks());
    invalidate_full(sw, sh);
}

static void window_minimize(window_t* w, int sw, int sh) {
    if (!w || !gui_wm_active(w) || gui_wm_minimized(w)) return;
    gui_wm_set_minimized(w, 1);
    invalidate_full(sw, sh);
}

static void window_restore_focus(window_t* w, int sw, int sh) {
    if (!w || !gui_wm_active(w)) return;
    gui_wm_set_minimized(w, 0);
    z_push_top(win_index(w));
    invalidate_full(sw, sh);
}

static void cycle_windows(int sw, int sh) {
    int count = gui_wm_stack_count();
    int current = -1;
    window_t* top = topmost();
    if (count <= 0) return;
    if (top) {
        for (int i = 0; i < count; i++) {
            if (gui_wm_stack_at(i) == top) {
                current = i;
                break;
            }
        }
    }
    for (int step = 1; step <= count; step++) {
        int pos = (current + step) % count;
        window_t* w = gui_wm_stack_at(pos);
        if (gui_wm_active(w)) {
            window_restore_focus(w, sw, sh);
            return;
        }
    }
}

static uint32_t gui_wait_deadline(uint32_t now) {
    uint32_t deadline = 0;
    int have_deadline = 0;

    if (gui_desktop_shell_deadline() &&
        !tick_due(now, gui_desktop_shell_deadline())) {
        deadline = gui_desktop_shell_deadline();
        have_deadline = 1;
    }

    if (gui_frame_pending() && !gui_frame_due(now)) {
        deadline = have_deadline
                 ? min_deadline(deadline, gui_frame_deadline())
                 : gui_frame_deadline();
        have_deadline = 1;
    }

    {
        uint32_t app_deadline = gui_framework_deadline(now);
        if (app_deadline == now) return now;
        if (app_deadline) {
            deadline = have_deadline ? min_deadline(deadline, app_deadline)
                                     : app_deadline;
            have_deadline = 1;
        }
    }

    return have_deadline ? deadline : 0u;
}

static int hover_key(int mx, int my) {
    window_t* w = hit_window_z(mx, my);

    if (!w) {
        int icon = gui_desktop_shell_icon_hit(mx, my);
        return icon >= 0 ? 1000 + icon : 0;
    }

    return 3000 + win_index(w);
}

static int invalidate_hover_key(int sw, int sh, int key) {
    if (key >= 1000 && key < 1002) {
        int idx = key - 1000;
        invalidate_rect(sw, sh, gui_desktop_shell_icon_bounds(idx));
        return 1;
    }

    return 0;
}

static void handle_click(int mx, int my, gui_fp_t mxf, gui_fp_t myf, int sw, int sh) {
    if (gui_desktop_shell_click(mx, my)) return;

    /* close button on any window? */
    window_t* w = hit_window_z(mx, my);
    if (w) {
        int cx = gui_wm_x(w) + gui_wm_width(w) - CLOSE_W - 2;
        int cy = gui_wm_y(w) + 2;
        if (point_in(mx, my, cx, cy, CLOSE_W - 2, TITLE_H - 4)) {
            (void)request_window_close(w, sw, sh);
            return;
        }
        if (point_in(mx, my, cx - TITLE_BUTTON_W, cy,
                     TITLE_BUTTON_W - 2, TITLE_H - 4)) {
            window_toggle_maximize(w, sw, sh);
            return;
        }
        if (point_in(mx, my, cx - TITLE_BUTTON_W * 2, cy,
                     TITLE_BUTTON_W - 2, TITLE_H - 4)) {
            window_minimize(w, sw, sh);
            return;
        }
        if (!gui_wm_maximized(w) && point_in(mx, my,
                     gui_wm_x(w) + gui_wm_width(w) - RESIZE_GRIP,
                     gui_wm_y(w) + gui_wm_height(w) - RESIZE_GRIP,
                     RESIZE_GRIP,
                     RESIZE_GRIP)) {
            window_t* old_top = topmost();
            if (old_top && old_top != w) invalidate_window(sw, sh, old_top);
            invalidate_window(sw, sh, w);
            z_push_top(win_index(w));
            g_drag = DRAG_RESIZE;
            g_drag_idx = win_index(w);
            g_resize_start_mx = mx;
            g_resize_start_my = my;
            g_resize_start_w = gui_wm_width(w);
            g_resize_start_h = gui_wm_height(w);
            return;
        }
        /* title bar: raise + start drag */
        if (point_in(mx, my, gui_wm_x(w), gui_wm_y(w), gui_wm_width(w), TITLE_H)) {
            uint32_t now = sys_get_ticks();
            if (g_last_title_click_window == win_index(w) &&
                (uint32_t)(now - g_last_title_click_tick) <=
                    SMALLOS_TIMER_HZ / 3u) {
                g_last_title_click_window = -1;
                g_last_title_click_tick = 0;
                window_toggle_maximize(w, sw, sh);
                return;
            }
            g_last_title_click_window = win_index(w);
            g_last_title_click_tick = now;
            window_t* old_top = topmost();
            if (old_top && old_top != w) invalidate_window(sw, sh, old_top);
            invalidate_window(sw, sh, w);
            z_push_top(win_index(w));
            if (gui_wm_maximized(w)) {
                gui_wm_set_geometry(w, gui_wm_restore_x(w), gui_wm_restore_y(w),
                                    gui_wm_restore_w(w), gui_wm_restore_h(w));
                gui_wm_set_maximized(w, 0);
                dispatch_window_resize(w, now);
            }
            g_drag = DRAG_MOVE;
            g_drag_idx = win_index(w);
            g_drag_dx_fp = mxf - fp_from_int(gui_wm_x(w));
            g_drag_dy_fp = myf - fp_from_int(gui_wm_y(w));
            g_drag_preview_x = gui_wm_x(w);
            g_drag_preview_y = gui_wm_y(w);
            g_drag_overlay_visible = 0;
            g_drag_overlay_x = gui_wm_x(w);
            g_drag_overlay_y = gui_wm_y(w);
            gui_request_frame_now(sys_get_ticks());
            return;
        }
        /* body click: raise */
        {
            window_t* old_top = topmost();
            if (old_top && old_top != w) invalidate_window(sw, sh, old_top);
            invalidate_window(sw, sh, w);
        }
        z_push_top(win_index(w));
        {
            gui_app_event_t event;
            memset(&event, 0, sizeof(event));
            event.type = GUI_APP_EVENT_POINTER_DOWN;
            event.x = mx - gui_wm_x(w);
            event.y = my - (gui_wm_y(w) + TITLE_H);
            event.width = gui_wm_width(w);
            event.height = gui_wm_height(w) - TITLE_H;
            event.buttons = SYS_MOUSE_BUTTON_LEFT;
            event.ticks = sys_get_ticks();
            apply_app_result(w, dispatch_app_event(w, &event), sw, sh);
        }
        return;
    }

    /* desktop icon? */
    int hi = gui_desktop_shell_icon_hit(mx, my);
    if (hi >= 0) {
        (void)gui_open_app(hi == 0 ? GUI_APP_FILES : GUI_APP_SHELL, 0);
        return;
    }
}

static void handle_wheel(int mx, int my, int wheel, int sw, int sh) {
    if (wheel == 0) return;

    window_t* w = hit_window_z(mx, my);
    if (!w) return;
    z_push_top(win_index(w));

    {
        gui_app_event_t event;
        memset(&event, 0, sizeof(event));
        event.type = GUI_APP_EVENT_WHEEL;
        event.x = mx - gui_wm_x(w);
        event.y = my - (gui_wm_y(w) + TITLE_H);
        event.width = gui_wm_width(w);
        event.height = gui_wm_height(w) - TITLE_H;
        event.wheel = wheel;
        event.ticks = sys_get_ticks();
        apply_app_result(w, dispatch_app_event(w, &event), sw, sh);
    }
}

static void shell_request_quit(void) {
    action_quit(g_screen_width, g_screen_height);
}

static void shell_minimize(gui_window_t* window) {
    window_minimize(window, g_screen_width, g_screen_height);
}

static void shell_restore_focus(gui_window_t* window) {
    window_restore_focus(window, g_screen_width, g_screen_height);
}

static void shell_invalidate_full(void) {
    invalidate_full(g_screen_width, g_screen_height);
}

static void shell_invalidate_taskbar(void) {
    invalidate_topbar(g_screen_width, g_screen_height);
}

static unsigned int shell_shown_presents(void) {
    return gui_compositor_metrics()->shown_presents;
}

static void framework_invalidate_rect(gui_rect_t rect_value) {
    invalidate_rect(g_screen_width, g_screen_height, rect_value);
}

static void framework_notice(const char* text, uint32_t until) {
    gui_desktop_shell_set_notice(text, until);
}

int gui_runtime_run(const gui_runtime_options_t* options) {
    gfx_context_t gfx;
    int rc;
    const char* initial_path = options ? options->initial_path : 0;
    g_diagnostics = options ? options->diagnostics : 0;

    u_puts("gui: starting (ESC or q to exit)\n");
    rc = gfx_open(&gfx);
    if (rc == -1) { u_puts("gui: framebuffer not available\n"); return 0; }
    if (rc == -2) { u_puts("gui: display is already in use\n");  return 1; }
    if (rc == -3) { u_puts("gui: unsupported display size\n");   return 1; }
    if (rc == -4) { u_puts("gui: out of memory opening display\n"); return 1; }
    if (rc < 0)   { u_puts("gui: could not open display\n");     return 1; }

    int sw = (int)gfx.backbuffer.width;
    int sh = (int)gfx.backbuffer.height;
    g_screen_width = sw;
    g_screen_height = sh;
    gui_fp_t mxf = fp_from_int(sw / 2);
    gui_fp_t myf = fp_from_int(sh / 2);
    int mx = fp_to_int_round(mxf);
    int my = fp_to_int_round(myf);

    gui_compositor_init(sw, sh, sys_get_ticks());
    gui_wm_init();
    {
        gui_modal_services_t modal_services = {
            modal_dispatch, modal_invalidate, modal_notice,
            modal_owner_active, 0
        };
        gui_modal_init(sw, sh, &modal_services);
    }
    {
        gui_native_ui_t native_ui = {
            draw_text, text_width, &GUI_WIDGET_THEME,
            COL_WIN_BG, COL_FRAME, COL_TEXT, COL_SUBTEXT,
            COL_HILIGHT, COL_HILIGHT_T
        };
        gui_native_apps_init(&native_ui);
    }
    {
        gui_framework_services_t framework_services = {
            framework_invalidate_rect, shell_invalidate_full,
            shell_invalidate_taskbar, framework_notice, 0
        };
        gui_framework_runtime_init(sw, sh, &framework_services);
    }
    {
        gui_desktop_shell_services_t shell_services = {
            gui_open_app, shell_request_quit, shell_minimize,
            shell_restore_focus, shell_invalidate_full,
            shell_invalidate_taskbar, shell_shown_presents
        };
        gui_desktop_shell_init(sw, sh, &shell_services);
    }

    /* drain any stale mouse delta */
    { sys_mouse_state_t m; (void)sys_mouse_read(&m); }

    int prev_left = 0;
    int dirty = 1;
    int cursor_dirty = 1;
    int presented_mx = mx;
    int presented_my = my;
    int last_hover = hover_key(mx, my);
    gui_preferences_load();
    if (initial_path) (void)gui_open_app(GUI_APP_EDITOR, initial_path);
    invalidate_full(sw, sh);
    g_startup_ram_bytes = sample_self_ram();

    while (!g_should_quit) {
        sys_input_event_t events[INPUT_BATCH];
        int got = 0;
        unsigned int input_flags = SYS_INPUT_FLAG_NONBLOCK;

        if (gui_desktop_shell_tick(sys_get_ticks())) dirty = 1;

        int n = read_input_coalesced(events, INPUT_BATCH, input_flags);
        if (perf_tick(sw, sh)) {
            dirty = 1;
        }
        if (n > 0) {
            got = 1;
            perf_note_input((unsigned int)n);

            for (int ei = 0; ei < n && !g_should_quit; ei++) {
                sys_input_event_t* ev = &events[ei];

                if (ev->type == SYS_INPUT_TYPE_KEY &&
                    (ev->flags & SYS_INPUT_KEY_PRESSED) != 0u) {
                    unsigned int a = ev->ascii & 0xFFu;
                    window_t* top = topmost();
                    unsigned int app_result = GUI_APP_RESULT_NONE;
                    if (gui_modal_active()) {
                        gui_app_event_t modal_event;
                        memset(&modal_event, 0, sizeof(modal_event));
                        modal_event.type = GUI_APP_EVENT_KEY;
                        modal_event.key = ev->key;
                        modal_event.ascii = a;
                        modal_event.modifiers = ev->flags;
                        modal_event.ticks = ev->ticks;
                        (void)gui_modal_event(&modal_event);
                        dirty = 1;
                        continue;
                    }
                    if ((ev->flags & SYS_INPUT_KEY_ALT) &&
                        ev->key == KEY_TAB) {
                        cycle_windows(sw, sh);
                        dirty = 1;
                        continue;
                    }
                    if ((ev->flags & SYS_INPUT_KEY_ALT) && top &&
                        ev->key == KEY_F9) {
                        window_minimize(top, sw, sh);
                        dirty = 1;
                        continue;
                    }
                    if ((ev->flags & SYS_INPUT_KEY_ALT) && top &&
                        ev->key == KEY_F10) {
                        window_toggle_maximize(top, sw, sh);
                        dirty = 1;
                        continue;
                    }
                    if ((ev->flags & SYS_INPUT_KEY_CTRL) &&
                        ev->key == KEY_ESC) {
                        gui_desktop_shell_toggle_start();
                        dirty = 1;
                        continue;
                    }
                    if (gui_desktop_shell_key(ev->key, a)) {
                        dirty = 1;
                        continue;
                    }
                    if (top) {
                        gui_app_event_t app_event;
                        memset(&app_event, 0, sizeof(app_event));
                        app_event.type = GUI_APP_EVENT_KEY;
                        app_event.width = gui_wm_width(top);
                        app_event.height = gui_wm_height(top) - TITLE_H;
                        app_event.key = ev->key;
                        app_event.ascii = a;
                        app_event.modifiers = ev->flags;
                        app_event.ticks = ev->ticks;
                        app_result = dispatch_app_event(top, &app_event);
                        apply_app_result(top, app_result, sw, sh);
                        if (g_dirty_count > 0) dirty = 1;
                    }
                    if (!(app_result & GUI_APP_RESULT_HANDLED)) {
                        if (a == 27u || ev->key == KEY_ESC ||
                            a == 'q' || a == 'Q') {
                            action_quit(sw, sh);
                        } else if ((a == 'x' || a == 'X') && top) {
                            (void)request_window_close(top, sw, sh);
                        } else if (a == 'f' || a == 'F') {
                            (void)gui_open_app(GUI_APP_FILES, 0);
                        } else if (a == 't' || a == 'T') {
                            (void)gui_open_app(GUI_APP_SHELL, 0);
                        } else if (a == 's' || a == 'S') {
                            (void)gui_open_app(GUI_APP_SYSTEM, 0);
                        } else if (a == 'c' || a == 'C') {
                            (void)gui_open_app(GUI_APP_CONFIG, 0);
                        } else if (a == 'a' || a == 'A') {
                            (void)gui_open_app(GUI_APP_ABOUT, 0);
                        }
                        if (g_dirty_count > 0) dirty = 1;
                    }
                } else if (ev->type == SYS_INPUT_TYPE_MOUSE) {
                    int old_mx = mx;
                    int old_my = my;
                    int old_hover = last_hover;

                    if (ev->flags & SYS_INPUT_MOUSE_ABSOLUTE) {
                        mxf = abs_to_screen_fp(ev->abs_x, sw - 1);
                        myf = abs_to_screen_fp(ev->abs_y, sh - 1);
                    } else {
                        mxf = fp_clamp_int(mxf + fp_from_int(ev->dx), 0, sw - 1);
                        myf = fp_clamp_int(myf + fp_from_int(ev->dy), 0, sh - 1);
                    }
                    mx = fp_to_int_round(mxf);
                    my = fp_to_int_round(myf);
                    if (mx != old_mx || my != old_my) {
                        cursor_dirty = 1;
                    }
                    int left_now =
                        (ev->buttons & SYS_MOUSE_BUTTON_LEFT) != 0;
                    if (gui_modal_active()) {
                        gui_app_event_t modal_event;
                        memset(&modal_event, 0, sizeof(modal_event));
                        modal_event.x = mx;
                        modal_event.y = my;
                        modal_event.buttons = ev->buttons;
                        modal_event.wheel = ev->wheel;
                        modal_event.ticks = ev->ticks;
                        if (ev->wheel)
                            modal_event.type = GUI_APP_EVENT_WHEEL;
                        else if (left_now && !prev_left)
                            modal_event.type = GUI_APP_EVENT_POINTER_DOWN;
                        else if (!left_now && prev_left)
                            modal_event.type = GUI_APP_EVENT_POINTER_UP;
                        else
                            modal_event.type = GUI_APP_EVENT_POINTER_MOVE;
                        (void)gui_modal_event(&modal_event);
                        prev_left = left_now;
                        dirty = 1;
                        continue;
                    }
                    if (ev->wheel != 0) {
                        handle_wheel(mx, my, ev->wheel, sw, sh);
                        dirty = 1;
                    }
                    if (left_now && !prev_left) {
                        handle_click(mx, my, mxf, myf, sw, sh);
                        if (g_dirty_count > 0) dirty = 1;
                    } else if (!left_now && prev_left) {
                        if (g_drag == DRAG_MOVE && g_drag_idx >= 0) {
                            window_t* w = gui_wm_at(g_drag_idx);
                            if (gui_wm_active(w)) {
                                if (g_drag_overlay_visible) {
                                    if (present_drag_preview(&gfx, drag_overlay_rect(), 0) < 0) {
                                        gfx_close(&gfx);
                                        u_puts("gui: present failed\n");
                                        return 1;
                                    }
                                    g_drag_overlay_visible = 0;
                                    cursor_dirty = 1;
                                }
                                invalidate_window(sw, sh, w);
                                gui_wm_set_geometry(w, g_drag_preview_x,
                                    g_drag_preview_y, gui_wm_width(w),
                                    gui_wm_height(w));
                                if (my <= 2) {
                                    gui_wm_set_geometry(w, gui_wm_restore_x(w),
                                        gui_wm_restore_y(w), gui_wm_restore_w(w),
                                        gui_wm_restore_h(w));
                                    gui_wm_set_maximized(w, 0);
                                    window_toggle_maximize(w, sw, sh);
                                } else if (mx <= 2) {
                                    window_snap(w, sw, sh, -1);
                                } else if (mx >= sw - 3) {
                                    window_snap(w, sw, sh, 1);
                                }
                                invalidate_window(sw, sh, w);
                                dirty = 1;
                            }
                        }
                        g_drag = DRAG_NONE;
                        g_drag_idx = -1;
                        gui_cancel_frame();
                    }
                    if (g_drag == DRAG_MOVE && g_drag_idx >= 0) {
                        window_t* w = gui_wm_at(g_drag_idx);
                        if (gui_wm_active(w)) {
                            gui_fp_t new_x_fp = fp_clamp_int(mxf - g_drag_dx_fp,
                                                             0,
                                                             sw - 32);
                            gui_fp_t new_y_fp = fp_clamp_int(myf - g_drag_dy_fp,
                                                             0,
                                                             sh - TASKBAR_H - TITLE_H);
                            int new_x = fp_to_int_round(new_x_fp);
                            int new_y = fp_to_int_round(new_y_fp);
                            if (new_x != g_drag_preview_x || new_y != g_drag_preview_y) {
                                g_drag_preview_x = new_x;
                                g_drag_preview_y = new_y;
                                cursor_dirty = 1;
                                gui_request_frame(sys_get_ticks());
                            }
                        }
                    }
                    if (g_drag == DRAG_RESIZE && g_drag_idx >= 0) {
                        window_t* w = gui_wm_at(g_drag_idx);
                        if (gui_wm_active(w)) {
                            const window_app_ops_t* ops = window_app_ops(gui_wm_app_id(w));
                            int max_w = sw - gui_wm_x(w);
                            int max_h = sh - TASKBAR_H - gui_wm_y(w);
                            int min_w = ops ? ops->min_width : WINDOW_MIN_W;
                            int min_h = ops ? ops->min_height : WINDOW_MIN_H;
                            int new_w;
                            int new_h;
                            if (max_w < min_w) max_w = min_w;
                            if (max_h < min_h) max_h = min_h;
                            new_w = clampi(g_resize_start_w + mx - g_resize_start_mx,
                                         min_w, max_w);
                            new_h = clampi(g_resize_start_h + my - g_resize_start_my,
                                         min_h, max_h);
                            if (new_w != gui_wm_width(w) || new_h != gui_wm_height(w)) {
                                gui_app_event_t resize_event;
                                invalidate_window(sw, sh, w);
                                gui_wm_set_geometry(w, gui_wm_x(w), gui_wm_y(w),
                                                    new_w, new_h);
                                memset(&resize_event, 0, sizeof(resize_event));
                                resize_event.type = GUI_APP_EVENT_RESIZE;
                                resize_event.width = gui_wm_width(w);
                                resize_event.height = gui_wm_height(w) - TITLE_H;
                                resize_event.ticks = ev->ticks;
                                (void)dispatch_app_event(w, &resize_event);
                                invalidate_window(sw, sh, w);
                                dirty = 1;
                            }
                        }
                    }
                    if (!left_now && prev_left) {
                        window_t* target = 0;
                        for (int i = 0; i < MAX_WINDOWS; i++) {
                            if (gui_wm_active(gui_wm_at(i)) && gui_wm_captured_control(gui_wm_at(i))) {
                                target = gui_wm_at(i);
                                break;
                            }
                        }
                        if (!target) target = hit_window_z(mx, my);
                        if (target) {
                            gui_app_event_t app_event;
                            memset(&app_event, 0, sizeof(app_event));
                            app_event.type = GUI_APP_EVENT_POINTER_UP;
                            app_event.x = mx - gui_wm_x(target);
                            app_event.y = my - (gui_wm_y(target) + TITLE_H);
                            app_event.width = gui_wm_width(target);
                            app_event.height = gui_wm_height(target) - TITLE_H;
                            app_event.ticks = ev->ticks;
                            apply_app_result(target,
                                dispatch_app_event(target, &app_event), sw, sh);
                            if (g_dirty_count > 0) dirty = 1;
                        }
                    } else if ((mx != old_mx || my != old_my) &&
                               g_drag == DRAG_NONE) {
                        window_t* target = 0;
                        for (int i = 0; i < MAX_WINDOWS; i++) {
                            if (gui_wm_active(gui_wm_at(i)) && gui_wm_captured_control(gui_wm_at(i))) {
                                target = gui_wm_at(i);
                                break;
                            }
                        }
                        if (!target) target = hit_window_z(mx, my);
                        if (target) {
                            gui_app_event_t app_event;
                            memset(&app_event, 0, sizeof(app_event));
                            app_event.type = GUI_APP_EVENT_POINTER_MOVE;
                            app_event.x = mx - gui_wm_x(target);
                            app_event.y = my - (gui_wm_y(target) + TITLE_H);
                            app_event.width = gui_wm_width(target);
                            app_event.height = gui_wm_height(target) - TITLE_H;
                            app_event.buttons = ev->buttons;
                            app_event.ticks = ev->ticks;
                            apply_app_result(target,
                                dispatch_app_event(target, &app_event), sw, sh);
                            if (g_dirty_count > 0) dirty = 1;
                        }
                    }
                    prev_left = left_now;
                    last_hover = hover_key(mx, my);
                    if (last_hover != old_hover) {
                        int hover_dirty = invalidate_hover_key(sw, sh, old_hover);
                        hover_dirty |= invalidate_hover_key(sw, sh, last_hover);
                        if (hover_dirty) dirty = 1;
                    }
                }
            }
        }

        if (dispatch_ready_app_fds(sw, sh)) dirty = 1;
        sync_window_focus(sw, sh);
        if (dispatch_due_app_ticks(sw, sh, sys_get_ticks())) dirty = 1;

        if (g_launch_kind != LAUNCH_NONE) {
            int launch_rc = run_queued_launch(&gfx, &sw, &sh);
            if (launch_rc < 0) return 1;
            gui_compositor_resize(sw, sh);
            gui_framework_runtime_resize(sw, sh);
            gui_modal_resize(sw, sh);
            mxf = fp_clamp_int(mxf, 0, sw - 1);
            myf = fp_clamp_int(myf, 0, sh - 1);
            mx = fp_to_int_round(mxf);
            my = fp_to_int_round(myf);
            presented_mx = mx;
            presented_my = my;
            last_hover = hover_key(mx, my);
            cursor_dirty = 1;
            dirty = 1;
            invalidate_full(sw, sh);
        }

        /* A logical event without pixel damage must not become a full repaint. */
        if (dirty && g_dirty_count == 0) {
            dirty = 0;
        }

        if (dirty) {
            if (g_drag_overlay_visible) {
                if (present_drag_preview(&gfx, drag_overlay_rect(), 0) < 0) {
                    gfx_close(&gfx);
                    u_puts("gui: present failed\n");
                    return 1;
                }
                g_drag_overlay_visible = 0;
                cursor_dirty = 1;
            }
            if (present_dirty_scene(&gfx, mx, my,
                                    presented_mx, presented_my) < 0) {
                gfx_close(&gfx);
                u_puts("gui: present failed\n");
                return 1;
            }
            dirty = 0;
            if (cursor_dirty) {
                if (present_cursor_move(&gfx, presented_mx, presented_my, mx, my) < 0) {
                    gfx_close(&gfx);
                    u_puts("gui: present failed\n");
                    return 1;
                }
                cursor_dirty = 0;
            }
            if (g_drag == DRAG_MOVE && g_drag_idx >= 0) {
                uint32_t now = sys_get_ticks();
                gui_request_frame_now(now);
                if (present_gui_frame(&gfx, mx, my, now) < 0) {
                    gfx_close(&gfx);
                    u_puts("gui: present failed\n");
                    return 1;
                }
            }
            last_hover = hover_key(mx, my);
            presented_mx = mx;
            presented_my = my;
        } else if (cursor_dirty) {
            if (g_drag == DRAG_MOVE && g_drag_idx >= 0) {
                if (present_cursor_move_with_drag_overlay(&gfx,
                                                          presented_mx,
                                                          presented_my,
                                                          mx,
                                                          my) < 0) {
                    gfx_close(&gfx);
                    u_puts("gui: present failed\n");
                    return 1;
                }
            } else if (present_cursor_move(&gfx, presented_mx, presented_my, mx, my) < 0) {
                gfx_close(&gfx);
                u_puts("gui: present failed\n");
                return 1;
            }
            cursor_dirty = 0;
            if (gui_frame_due(sys_get_ticks())) {
                if (present_gui_frame(&gfx, mx, my, sys_get_ticks()) < 0) {
                    gfx_close(&gfx);
                    u_puts("gui: present failed\n");
                    return 1;
                }
            }
            presented_mx = mx;
            presented_my = my;
        } else if (gui_frame_due(sys_get_ticks())) {
            if (present_gui_frame(&gfx, mx, my, sys_get_ticks()) < 0) {
                gfx_close(&gfx);
                u_puts("gui: present failed\n");
                return 1;
            }
        }

        if (!got) {
            uint32_t now = sys_get_ticks();
            uint32_t deadline = gui_wait_deadline(now);
            struct pollfd fds[MAX_WINDOWS];
            unsigned int fd_count = app_poll_fds(fds, 0, MAX_WINDOWS);
            if (deadline == 0u || !tick_due(now, deadline)) {
                int wait_result = smallos_input_fd_wait_until(fds, fd_count,
                                                               deadline);
                if (wait_result & SYS_INPUT_FD_WAIT_READY) {
                    gui_compositor_note_fd_wakeup();
                }
                perf_resume_from_idle();
            } else {
                sys_yield();
            }
        }
    }

    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (gui_wm_active(gui_wm_at(i))) close_window(gui_wm_at(i));
    }
    gfx_close(&gfx);
    if (g_diagnostics) diagnostics_print();
    u_puts("gui: exiting\n");
    return 0;
}
