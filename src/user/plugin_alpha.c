extern int plugin_helper_add(int value, int amount);

static int g_state;

int smallos_plugin_abi(void) {
    return 1;
}

const char* smallos_plugin_name(void) {
    return "alpha";
}

int smallos_plugin_run(int input) {
    g_state++;
    return plugin_helper_add(input, 100) + g_state;
}

int smallos_plugin_state(void) {
    return g_state;
}
