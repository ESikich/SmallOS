#include "preferences.h"

#include "native_apps.h"
#include "user_lib.h"

#define GUI_CONFIG_PATH "/etc/gui.conf"
#define GUI_CONFIG_MAX 256u

static int performance_visible;

static unsigned int text_length(const char* text) {
    unsigned int length = 0;
    while (text && text[length]) length++;
    return length;
}

static void text_copy(char* out, const char* text, unsigned int capacity) {
    unsigned int i = 0;
    while (text && text[i] && i + 1u < capacity) {
        out[i] = text[i];
        i++;
    }
    if (capacity) out[i] = 0;
}

static void text_append(char* out, const char* text, unsigned int capacity) {
    unsigned int used = text_length(out);
    while (text && *text && used + 1u < capacity) out[used++] = *text++;
    if (capacity) out[used] = 0;
}

static int starts_with(const char* text, const char* prefix) {
    while (*prefix) if (*text++ != *prefix++) return 0;
    return 1;
}

static const char* skip_spaces(const char* text) {
    while (*text == ' ' || *text == '\t') text++;
    return text;
}

static void parse_line(const char* line) {
    const char* text = skip_spaces(line);
    if (!*text || *text == '#' || *text == ';') return;
    if (starts_with(text, "perf_visible")) {
        text = skip_spaces(text + 12);
        if (*text != '=') return;
        text = skip_spaces(text + 1);
        if (*text == '0') performance_visible = 0;
        else if (*text == '1') performance_visible = 1;
        return;
    }
    {
        const char* equals = text;
        char key[32];
        unsigned int length = 0;
        while (*equals && *equals != '=') equals++;
        if (*equals != '=') return;
        while (text < equals && *text != ' ' && *text != '\t' &&
               length + 1u < sizeof(key)) key[length++] = *text++;
        key[length] = 0;
        gui_native_network_pref_set(key, skip_spaces(equals + 1));
    }
}

void gui_preferences_load(void) {
    char buffer[GUI_CONFIG_MAX + 1u];
    char line[64];
    unsigned int line_length = 0;
    int fd = sys_open(GUI_CONFIG_PATH);
    int count;
    if (fd < 0) return;
    count = sys_fread(fd, buffer, GUI_CONFIG_MAX);
    sys_close(fd);
    if (count <= 0) return;
    buffer[count] = 0;
    for (int i = 0; i < count; i++) {
        if (buffer[i] == '\r') continue;
        if (buffer[i] == '\n') {
            line[line_length] = 0;
            parse_line(line);
            line_length = 0;
        } else if (line_length + 1u < sizeof(line)) {
            line[line_length++] = buffer[i];
        }
    }
    if (line_length) {
        line[line_length] = 0;
        parse_line(line);
    }
}

void gui_preferences_save(void) {
    char buffer[GUI_CONFIG_MAX];
    int fd;
    text_copy(buffer, "perf_visible=", sizeof(buffer));
    text_append(buffer, performance_visible ? "1\n" : "0\n", sizeof(buffer));
    text_append(buffer, "theme=retro\nnetwork_address=", sizeof(buffer));
    text_append(buffer, gui_native_network_pref_get("network_address"),
                sizeof(buffer));
    text_append(buffer, "\nnetwork_prefix=", sizeof(buffer));
    text_append(buffer, gui_native_network_pref_get("network_prefix"),
                sizeof(buffer));
    text_append(buffer, "\nnetwork_gateway=", sizeof(buffer));
    text_append(buffer, gui_native_network_pref_get("network_gateway"),
                sizeof(buffer));
    text_append(buffer, "\nnetwork_dns=", sizeof(buffer));
    text_append(buffer, gui_native_network_pref_get("network_dns"),
                sizeof(buffer));
    text_append(buffer, "\n", sizeof(buffer));
    fd = sys_open_mode(GUI_CONFIG_PATH, SYS_OPEN_MODE_WRITE |
                       SYS_OPEN_MODE_CREATE | SYS_OPEN_MODE_TRUNC);
    if (fd < 0) return;
    (void)sys_writefd(fd, buffer, text_length(buffer));
    (void)sys_fsync(fd);
    (void)sys_close(fd);
}

int gui_preferences_performance_visible(void) {
    return performance_visible;
}

void gui_preferences_toggle_performance(void) {
    performance_visible = !performance_visible;
    gui_preferences_save();
}
