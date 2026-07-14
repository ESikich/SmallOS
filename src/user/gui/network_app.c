#include "native_apps_internal.h"

/* ---------------- Network ---------------- */

typedef struct network_state {
    sys_netinfo_t info;
    gui_text_input_t ip;
    gui_text_input_t prefix;
    gui_text_input_t gateway;
    gui_text_input_t dns;
    char status[80];
} network_state_t;

enum {
    NETWORK_CONTROL_ADDRESS = 1,
    NETWORK_CONTROL_PREFIX,
    NETWORK_CONTROL_GATEWAY,
    NETWORK_CONTROL_DNS,
    NETWORK_CONTROL_APPLY,
    NETWORK_CONTROL_DHCP,
    NETWORK_CONTROL_RELEASE
};

static void ip_text(unsigned int ip, char* out) {
    char n[16]; out[0] = 0;
    for (int shift = 24; shift >= 0; shift -= 8) {
        uint_text((ip >> shift) & 255u, n); append_text(out, n, 32);
        if (shift) append_text(out, ".", 32);
    }
}

static void network_load(network_state_t* state) {
    char text[32], number[16];
    gui_text_input_init(&state->ip, "");
    gui_text_input_init(&state->prefix, "");
    gui_text_input_init(&state->gateway, "");
    gui_text_input_init(&state->dns, "");
    if (sys_netinfo(&state->info) < 0) {
        copy_text(state->status, "Network information unavailable", sizeof(state->status));
        return;
    }
    if (state->info.ipv4_configured) {
        ip_text(state->info.ip, text); gui_text_input_init(&state->ip, text);
        unsigned int mask = state->info.netmask, prefix = 0;
        if (!gui_ipv4_mask_prefix(mask, &prefix)) {
            copy_text(state->status, "Invalid noncontiguous netmask",
                      sizeof(state->status));
        } else {
            uint_text(prefix, number); gui_text_input_init(&state->prefix, number);
        }
        if (state->info.gateway) ip_text(state->info.gateway, text); else text[0] = 0;
        gui_text_input_init(&state->gateway, text);
        if (state->info.dns) ip_text(state->info.dns, text); else text[0] = 0;
        gui_text_input_init(&state->dns, text);
    }
}

static void network_open(gui_app_context_t* context, const char* argument) {
    network_state_t* state = gui_app_state(context); (void)argument;
    memset(state, 0, sizeof(*state)); network_load(state);
    if (!state->info.ipv4_configured && g_pref_address[0]) {
        gui_text_input_init(&state->ip, g_pref_address);
        gui_text_input_init(&state->prefix, g_pref_prefix);
        gui_text_input_init(&state->gateway, g_pref_gateway);
        gui_text_input_init(&state->dns, g_pref_dns);
    }
}

static gui_rect_t network_field(int index) {
    return gui_rect_make(78, 86 + index * 24, index == 1 ? 48 : 142, 18);
}

static void network_draw(gfx_surface_t* s, gui_app_context_t* context,
                         int mx, int my) {
    network_state_t* state = gui_app_state(context);
    int bx = 0, by = 0, width = (int)s->width, bh = (int)s->height;
    char text[96], n[16], ip[32];
    int focus = gui_app_focused_control(context);
    int captured = gui_app_captured_control(context);
    (void)mx; (void)my;
    gui_canvas_fill_rect(s, bx, by, width, bh, g_ui.window_bg);
    copy_text(text, "Driver ", sizeof(text)); append_text(text, state->info.net_driver, sizeof(text));
    append_text(text, state->info.net_link_up ? "  Up  MAC " : "  Down  MAC ", sizeof(text));
    for (int i = 0; i < 6; i++) {
        static const char hex[] = "0123456789ABCDEF";
        char byte[4] = {hex[state->info.mac[i] >> 4],
                        hex[state->info.mac[i] & 15u], i == 5 ? 0 : ':', 0};
        append_text(text, byte, sizeof(text));
    }
    g_ui.draw_text(s, bx + 6, by + 7, text, g_ui.text);
    copy_text(text, "Packets TX ", sizeof(text)); uint_text(state->info.nic_tx_packets, n); append_text(text,n,sizeof(text));
    append_text(text, "  RX ", sizeof(text)); uint_text(state->info.nic_rx_packets,n); append_text(text,n,sizeof(text));
    append_text(text, "  Err ", sizeof(text));
    uint_text(state->info.nic_tx_errors + state->info.nic_rx_errors,n); append_text(text,n,sizeof(text));
    g_ui.draw_text(s, bx + 6, by + 22, text, g_ui.subtext);
    copy_text(text, "Sockets ", sizeof(text)); uint_text(state->info.used_sockets,n); append_text(text,n,sizeof(text));
    append_text(text,"/",sizeof(text)); uint_text(state->info.max_sockets,n); append_text(text,n,sizeof(text));
    append_text(text, "  DHCP ", sizeof(text));
    if (state->info.dhcp_server) { ip_text(state->info.dhcp_server, ip); append_text(text,ip,sizeof(text)); }
    else append_text(text,"none",sizeof(text));
    append_text(text, "  Lease ", sizeof(text)); uint_text(state->info.lease_seconds,n); append_text(text,n,sizeof(text));
    g_ui.draw_text(s, bx + 6, by + 37, text, g_ui.subtext);
    if (state->info.ipv4_configured) {
        ip_text(state->info.ip, ip); copy_text(text,"Current ",sizeof(text)); append_text(text,ip,sizeof(text));
        append_text(text,"  GW ",sizeof(text));
        if (state->info.gateway) { ip_text(state->info.gateway,ip); append_text(text,ip,sizeof(text)); }
        else append_text(text,"none",sizeof(text));
        append_text(text,"  DNS ",sizeof(text));
        if (state->info.dns) { ip_text(state->info.dns,ip); append_text(text,ip,sizeof(text)); }
        else append_text(text,"none",sizeof(text));
    }
    else copy_text(text, "Current unconfigured", sizeof(text));
    g_ui.draw_text(s, bx + 6, by + 54, text, g_ui.text);
    const char* labels[] = {"Address", "Prefix", "Gateway", "DNS"};
    gui_text_input_t* inputs[] = {&state->ip,&state->prefix,&state->gateway,&state->dns};
    for (int i = 0; i < 4; i++) {
        gui_rect_t f = network_field(i);
        g_ui.draw_text(s, bx + 6, by + 92 + i * 24, labels[i], g_ui.text);
        gui_widget_text_field(s, f, inputs[i]->text, inputs[i]->cursor,
                              (gui_widget_state_t){0,0,focus == i + 1,0},
                              g_ui.widget_theme, g_ui.draw_text);
    }
    gui_widget_button(s, gui_rect_make(bx + 6, by + 186, 64, 20), "Apply",
                      (gui_widget_state_t){0,captured == NETWORK_CONTROL_APPLY,
                                           focus == NETWORK_CONTROL_APPLY,0},
                      g_ui.widget_theme,g_ui.draw_text);
    gui_widget_button(s, gui_rect_make(bx + 76, by + 186, 72, 20), "DHCP",
                      (gui_widget_state_t){0,captured == NETWORK_CONTROL_DHCP,
                                           focus == NETWORK_CONTROL_DHCP,0},
                      g_ui.widget_theme,g_ui.draw_text);
    gui_widget_button(s, gui_rect_make(bx + 154, by + 186, 72, 20), "Release",
                      (gui_widget_state_t){0,captured == NETWORK_CONTROL_RELEASE,
                                           focus == NETWORK_CONTROL_RELEASE,0},
                      g_ui.widget_theme,g_ui.draw_text);
    g_ui.draw_text(s, bx + 6, by + bh - 12, state->status, g_ui.subtext);
}

static int network_apply(network_state_t* state) {
    gui_ipv4_config_t config;
    gui_ipv4_validation_t validation;
    sys_net_op_request_t request;
    validation = gui_ipv4_validate(state->ip.text, state->prefix.text,
                                   state->gateway.text, state->dns.text,
                                   &config);
    if (validation == GUI_IPV4_NETWORK_OR_BROADCAST)
        copy_text(state->status, "Address is network/broadcast", sizeof(state->status));
    else if (validation == GUI_IPV4_GATEWAY_OUTSIDE_SUBNET)
        copy_text(state->status, "Gateway is outside subnet", sizeof(state->status));
    else if (validation != GUI_IPV4_VALID)
        copy_text(state->status, "Invalid IPv4 configuration", sizeof(state->status));
    if (validation != GUI_IPV4_VALID) return 0;
    memset(&request, 0, sizeof(request)); request.op = SYS_NET_OP_CONFIGURE;
    request.target_ip = config.address; request.netmask = config.netmask;
    request.gateway = config.gateway; request.dns = config.dns;
    if (sys_net_op(&request) <= 0) { copy_text(state->status,"Apply failed",sizeof(state->status)); return 0; }
    copy_text(g_pref_address, state->ip.text, sizeof(g_pref_address));
    copy_text(g_pref_prefix, state->prefix.text, sizeof(g_pref_prefix));
    copy_text(g_pref_gateway, state->gateway.text, sizeof(g_pref_gateway));
    copy_text(g_pref_dns, state->dns.text, sizeof(g_pref_dns));
    gui_preferences_save();
    copy_text(state->status,"Static configuration applied",sizeof(state->status)); network_load(state); return 1;
}

static void network_activate(network_state_t* state, int control) {
    sys_net_op_request_t request;
    if (control == NETWORK_CONTROL_APPLY) {
        (void)network_apply(state);
        return;
    }
    memset(&request, 0, sizeof(request));
    if (control == NETWORK_CONTROL_DHCP) {
        request.op = SYS_NET_OP_DHCP;
        copy_text(state->status,
                  sys_net_op(&request) > 0 ? "DHCP lease acquired" : "DHCP failed",
                  sizeof(state->status));
        network_load(state);
    } else if (control == NETWORK_CONTROL_RELEASE) {
        request.op = SYS_NET_OP_CLEAR_CONFIG;
        copy_text(state->status,
                  sys_net_op(&request) > 0 ? "Configuration cleared" : "Release failed",
                  sizeof(state->status));
        network_load(state);
    }
}

static unsigned int network_event(gui_app_context_t* context,
                                  const gui_app_event_t* event) {
    network_state_t* state = gui_app_state(context);
    if (event->type == GUI_APP_EVENT_TICK) {
        sys_netinfo(&state->info);
        int width = 0;
        gui_app_client_size(context, &width, 0);
        gui_app_invalidate(context, 0, 0, width, 74);
        return GUI_APP_RESULT_HANDLED;
    }
    if (event->type == GUI_APP_EVENT_POINTER_DOWN) {
        for (int i = 0; i < 4; i++) if (inside(event->x,event->y,network_field(i))) {
            gui_app_focus_control(context, i + 1);
            return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
        }
        int control = 0;
        if (inside(event->x,event->y,gui_rect_make(6,186,64,20)))
            control = NETWORK_CONTROL_APPLY;
        else if (inside(event->x,event->y,gui_rect_make(76,186,72,20)))
            control = NETWORK_CONTROL_DHCP;
        else if (inside(event->x,event->y,gui_rect_make(154,186,72,20)))
            control = NETWORK_CONTROL_RELEASE;
        if (!control) return GUI_APP_RESULT_NONE;
        gui_app_focus_control(context, control);
        gui_app_capture_pointer(context, control);
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_POINTER_UP &&
        gui_app_captured_control(context) >= NETWORK_CONTROL_APPLY) {
        int control = gui_app_captured_control(context);
        gui_rect_t bounds = control == NETWORK_CONTROL_APPLY
            ? gui_rect_make(6,186,64,20)
            : control == NETWORK_CONTROL_DHCP
            ? gui_rect_make(76,186,72,20)
            : gui_rect_make(154,186,72,20);
        gui_app_release_pointer(context);
        if (inside(event->x, event->y, bounds)) network_activate(state, control);
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_KEY && event->key == KEY_TAB) {
        int current = gui_app_focused_control(context);
        int next = gui_widget_focus_next(current > 0 ? current - 1 : -1,
            NETWORK_CONTROL_RELEASE,
            (event->modifiers & SYS_INPUT_KEY_SHIFT) != 0, 0);
        gui_app_focus_control(context, next + 1);
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_KEY &&
        gui_app_focused_control(context) >= NETWORK_CONTROL_APPLY &&
        (event->key == KEY_ENTER || event->key == KEY_SPACE)) {
        network_activate(state, gui_app_focused_control(context));
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_KEY &&
        gui_app_focused_control(context) >= NETWORK_CONTROL_ADDRESS &&
        gui_app_focused_control(context) <= NETWORK_CONTROL_DNS) {
        gui_text_input_t* inputs[] = {&state->ip,&state->prefix,&state->gateway,&state->dns};
        gui_text_input_t* input = inputs[gui_app_focused_control(context) - 1]; int changed = 0;
        if (event->key == KEY_LEFT) changed=gui_text_input_command(input,GUI_TEXT_INPUT_LEFT);
        else if (event->key == KEY_RIGHT) changed=gui_text_input_command(input,GUI_TEXT_INPUT_RIGHT);
        else if (event->key == KEY_HOME) changed=gui_text_input_command(input,GUI_TEXT_INPUT_HOME);
        else if (event->key == KEY_END) changed=gui_text_input_command(input,GUI_TEXT_INPUT_END);
        else if (event->key == KEY_BACKSPACE) changed=gui_text_input_command(input,GUI_TEXT_INPUT_BACKSPACE);
        else if (event->key == KEY_DELETE) changed=gui_text_input_command(input,GUI_TEXT_INPUT_DELETE);
        else if (event->key == KEY_ENTER) changed=network_apply(state);
        else if ((event->ascii >= '0' && event->ascii <= '9') || event->ascii == '.') changed=gui_text_input_insert(input,(char)event->ascii);
        else return GUI_APP_RESULT_HANDLED;
        return GUI_APP_RESULT_HANDLED | (changed ? GUI_APP_RESULT_REDRAW : 0);
    }
    return GUI_APP_RESULT_NONE;
}

static const gui_app_descriptor_t NETWORK_DESCRIPTOR = {
    "Network", sizeof(network_state_t), 420, 270, 320, 250, SMALLOS_TIMER_HZ,
    network_open, 0, network_draw, network_event, GUI_APP_NETWORK, 0,
    "network", "Network", 5, 1
};

const gui_app_descriptor_t* gui_network_app_descriptor(void) {
    return &NETWORK_DESCRIPTOR;
}
