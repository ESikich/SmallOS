#include "gui/region.h"
#include "gui/window.h"
#include "gui/canvas.h"
#include "gui/cursor.h"
#include "gui/damage.h"
#include "gui/layout.h"
#include "gui/widgets.h"
#include "gui/app_event.h"
#include "gui/framework.h"
#include "gui/desktop_model.h"
#include "gui/network_model.h"
#include "gui/occlusion.h"
#include "gui/tasks_model.h"
#include "gui/viewer_model.h"
#include "editor_model.h"

extern int printf(const char* format, ...);

static int failures;

static void check(const char* name, int condition) {
    printf("[gui-unit] %-34s %s\n", name, condition ? "PASS" : "FAIL");
    if (!condition) failures++;
}

static int rect_equal(gui_rect_t a, gui_rect_t b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

static void test_regions(void) {
    gui_rect_t a = gui_rect_make(-4, 2, 10, 8);
    gui_rect_t b = gui_rect_make(5, 4, 8, 8);
    gui_rect_t pieces[4];
    int piece_count;
    check("region clip", rect_equal(gui_rect_clip(a, 20, 20),
                                     gui_rect_make(0, 2, 6, 8)));
    check("region intersection", gui_rect_intersects(a, b));
    check("region compact merge", gui_rect_should_merge(a, b));
    check("region union", rect_equal(gui_rect_union(a, b),
                                      gui_rect_make(-4, 2, 17, 10)));
    piece_count = gui_rect_exclude(gui_rect_make(0, 0, 20, 20),
                                   gui_rect_make(8, 6, 4, 8), pieces);
    check("region excludes cursor footprint",
          piece_count == 4 &&
          rect_equal(pieces[0], gui_rect_make(0, 0, 20, 6)) &&
          rect_equal(pieces[1], gui_rect_make(0, 14, 20, 6)) &&
          rect_equal(pieces[2], gui_rect_make(0, 6, 8, 8)) &&
          rect_equal(pieces[3], gui_rect_make(12, 6, 8, 8)));
    piece_count = gui_rect_exclude(gui_rect_make(0, 0, 5, 5),
                                   gui_rect_make(10, 10, 2, 2), pieces);
    check("region preserves non-overlap",
          piece_count == 1 && rect_equal(pieces[0], gui_rect_make(0, 0, 5, 5)));
    {
        gui_damage_t damage;
        gui_damage_clear(&damage);
        gui_damage_add(&damage, gui_rect_make(0, 0, 4, 4), 100, 100);
        gui_damage_add(&damage, gui_rect_make(4, 0, 4, 4), 100, 100);
        check("damage merges neighbors",
              damage.count == 1 && damage.rects[0].w == 8);
        gui_damage_full(&damage, 100, 80);
        gui_damage_add(&damage, gui_rect_make(2, 2, 2, 2), 100, 80);
        check("full damage dominates",
              damage.full && damage.count == 1 && damage.rects[0].w == 100);
    }
}

static void test_window_stack(void) {
    gui_window_stack_t stack;
    gui_window_stack_init(&stack);
    gui_window_stack_raise(&stack, 2);
    gui_window_stack_raise(&stack, 5);
    gui_window_stack_raise(&stack, 2);
    check("window raise removes duplicate",
          gui_window_stack_count(&stack) == 2 &&
          gui_window_stack_top(&stack) == 2 &&
          gui_window_stack_at(&stack, 0) == 5);
    gui_window_stack_remove(&stack, 2);
    check("window remove", gui_window_stack_top(&stack) == 5);
}

static void test_registry_and_desktop(void) {
    static const gui_app_descriptor_t later = {
        .title = "Later", .id = GUI_APP_EDITOR,
        .launcher_label = "Later", .launcher_order = 8, .show_in_start = 1
    };
    static const gui_app_descriptor_t first = {
        .title = "First", .id = GUI_APP_FILES,
        .launcher_label = "First", .launcher_order = 1, .show_in_start = 1
    };
    gui_taskbar_layout_t layout;
    const char* labels[] = {"Files", "Editor", "Shell"};
    gui_app_registry_reset();
    check("registry accepts unique apps",
          gui_app_registry_add(&later) && gui_app_registry_add(&first) &&
          !gui_app_registry_add(&first));
    check("registry lookup and order",
          gui_app_registry_find(GUI_APP_EDITOR) == &later &&
          gui_app_registry_launcher_at(0) == &first &&
          gui_app_registry_launcher_count() == 2);
    layout = gui_taskbar_layout(640, 8);
    check("taskbar paging at 640x480",
          layout.paging && layout.button_width == 56 &&
          layout.per_page > 0 && layout.page_count > 1);
    layout = gui_taskbar_layout(1024, 4);
    check("taskbar expands at 1024x768",
          !layout.paging && layout.button_width == 120 && layout.per_page == 4);
    layout = gui_taskbar_layout(800, 8);
    check("taskbar layout at 800x600",
          !layout.paging && layout.button_width >= 56 &&
          layout.button_width <= 120);
    check("taskbar page clamp",
          gui_taskbar_clamp_page(gui_taskbar_layout(640, 8), 99) == 1);
    check("start wraps navigation",
          gui_start_move_selection(0, 3, -1) == 2 &&
          gui_start_move_selection(2, 3, 1) == 0);
    check("start first-letter selection",
          gui_start_first_letter(labels, 3, 's') == 2);
}

static void test_occlusion(void) {
    gui_rect_t output[GUI_VISIBLE_REGION_CAPACITY];
    gui_rect_t cuts[] = {
        {4, 0, 4, 10},
        {0, 4, 10, 2},
    };
    int count = gui_visible_regions(gui_rect_make(0, 0, 10, 10),
                                    cuts, 2, output,
                                    GUI_VISIBLE_REGION_CAPACITY);
    int area = 0;
    for (int i = 0; i < count; i++) area += output[i].w * output[i].h;
    check("occlusion subtracts multiple windows", count == 4 && area == 48);
    check("occlusion fully covered", gui_visible_regions(
        gui_rect_make(0, 0, 10, 10),
        (gui_rect_t[]){{-1, -1, 20, 20}}, 1,
        output, GUI_VISIBLE_REGION_CAPACITY) == 0);
}

static void test_native_models(void) {
    int width, height;
    gui_viewer_geometry_t geometry = {400, 300, 999, -999};
    gui_ipv4_config_t config;
    unsigned int prefix;
    gui_task_model_row_t rows[] = {
        {7, 2, 1, 4, 100, 20, "beta"},
        {2, 1, 1, 9, 80, 10, "Alpha"},
    };
    gui_viewer_scaled_size(800, 400, 300, 200, 0, &width, &height);
    check("viewer fit scaling", width == 300 && height == 150);
    gui_viewer_scaled_size(800, 400, 300, 200, 100, &width, &height);
    check("viewer 100 percent", width == 800 && height == 400);
    gui_viewer_clamp_pan(&geometry, 200, 100);
    check("viewer pan clamp", geometry.pan_x == 100 && geometry.pan_y == -100);
    check("IPv4 validates static config",
          gui_ipv4_validate("192.168.2.10", "24", "192.168.2.1", "",
                            &config) == GUI_IPV4_VALID &&
          config.netmask == 0xffffff00u);
    check("IPv4 rejects network host",
          gui_ipv4_validate("10.0.0.0", "24", "", "", 0) ==
          GUI_IPV4_NETWORK_OR_BROADCAST);
    check("IPv4 rejects outside gateway",
          gui_ipv4_validate("10.0.0.2", "24", "10.0.1.1", "", 0) ==
          GUI_IPV4_GATEWAY_OUTSIDE_SUBNET);
    check("IPv4 rejects noncontiguous mask",
          !gui_ipv4_mask_prefix(0xff00ff00u, &prefix));
    gui_tasks_sort(rows, 2, 3, 1);
    check("tasks default CPU sort", rows[0].pid == 2);
    gui_tasks_sort(rows, 2, 1, 0);
    check("tasks case-insensitive name sort", rows[0].pid == 2);
    check("tasks process protection",
          gui_tasks_is_protected(&rows[0], 2) &&
          !gui_tasks_is_protected(&rows[1], 99));
}

static void test_widgets_and_canvas(void) {
    unsigned int pixels[64];
    gfx_surface_t surface = {8, 8, 8, pixels};
    gui_rect_t thumb;
    gui_text_input_t input;
    gui_vlayout_t layout;
    gui_rect_t cell;
    unsigned char enabled[4] = {1, 0, 1, 1};
    for (int i = 0; i < 64; i++) pixels[i] = 0;
    gui_canvas_set_clip(gui_rect_make(2, 2, 3, 3));
    gui_canvas_fill_rect(&surface, 0, 0, 8, 8, 7);
    gui_canvas_clear_clip();
    check("canvas clip excludes outside",
          pixels[0] == 0 && pixels[2 + 2 * 8] == 7 && pixels[5 + 5 * 8] == 0);
    pixels[0] = 0x123456u;
    gui_cursor_draw(&surface, 0, 0);
    check("cursor module draws overlay", pixels[0] == 0);
    check("widget hit bounds",
          gui_widget_hit(gui_rect_make(4, 4, 8, 8), 4, 4) &&
          !gui_widget_hit(gui_rect_make(4, 4, 8, 8), 12, 12));
    thumb = gui_widget_scroll_thumb(gui_rect_make(0, 0, 10, 100), 100, 20, 40);
    check("scroll thumb geometry",
          thumb.x == 2 && thumb.y == 40 && thumb.w == 6 && thumb.h == 20);
    check("scroll drag maps offset",
          gui_widget_scroll_offset(gui_rect_make(0, 0, 10, 100),
                                   100, 20, 70, 10) == 60);
    gui_vlayout_begin(&layout, gui_rect_make(10, 20, 100, 80), 4);
    check("vertical layout advances",
          rect_equal(gui_vlayout_take(&layout, 12),
                     gui_rect_make(10, 20, 100, 12)) &&
          rect_equal(gui_vlayout_take(&layout, 10),
                     gui_rect_make(10, 36, 100, 10)));
    cell = gui_layout_cell(gui_rect_make(0, 0, 100, 20), 3, 5, 1);
    check("row layout cells", cell.x == 35 && cell.w == 30);
    gui_text_input_init(&input, "ac");
    gui_text_input_command(&input, GUI_TEXT_INPUT_LEFT);
    gui_text_input_insert(&input, 'b');
    gui_text_input_command(&input, GUI_TEXT_INPUT_BACKSPACE);
    check("text input editing",
          input.length == 2 && input.cursor == 1 &&
          input.text[0] == 'a' && input.text[1] == 'c');
    check("focus traversal skips disabled",
          gui_widget_focus_next(0, 4, 0, enabled) == 2 &&
          gui_widget_focus_next(0, 4, 1, enabled) == 3);
    for (int i = 0; i < 64; i++) pixels[i] = 0;
    {
        gui_widget_theme_t theme = {1, 2, 3, 4, 5, 6, 7};
        gui_widget_progress(&surface, gui_rect_make(0, 0, 8, 4),
                            1, 2, &theme);
    }
    check("progress clamps and fills", pixels[1 + 1 * 8] == 7);
}

static void test_editor_model(void) {
    editor_model_t model;
    uint32_t row = 1;
    uint32_t column = 0;
    editor_model_init(&model, "/tmp/unit.txt");
    check("editor insert line",
          editor_model_insert_line(&model, 0, "abc") &&
          editor_model_insert_line(&model, 1, "def"));
    check("editor insert character",
          editor_model_insert_char(&model, 0, 1, 'X') &&
          model.lines[0][0] == 'a' && model.lines[0][1] == 'X');
    check("editor split line",
          editor_model_split_line(&model, 0, 2) && model.count == 3 &&
          model.lines[1][0] == 'b');
    check("editor backspace joins",
          editor_model_backspace(&model, &row, &column) &&
          row == 0 && column == 2 && model.count == 2);
    check("editor delete joins",
          editor_model_delete(&model, 0,
                              editor_model_line_length(&model, 0)) &&
          model.count == 1);
    editor_model_clear(&model);
    editor_model_insert_line(&model, 0, "abc");
    editor_model_insert_line(&model, 1, "def");
    editor_model_insert_line(&model, 2, "ghi");
    check("editor delete selection",
          editor_model_delete_range(&model, 0, 1, 2, 1) &&
          model.count == 1 && model.lines[0][0] == 'a' &&
          model.lines[0][1] == 'h' && model.lines[0][2] == 'i' &&
          model.lines[0][3] == '\0');
    check("event result flags",
          (GUI_APP_RESULT_REDRAW | GUI_APP_RESULT_HANDLED) !=
          GUI_APP_RESULT_CLOSE);
    editor_model_destroy(&model);
}

int main(void) {
    test_regions();
    test_window_stack();
    test_registry_and_desktop();
    test_occlusion();
    test_widgets_and_canvas();
    test_native_models();
    test_editor_model();
    return failures ? 1 : 0;
}
