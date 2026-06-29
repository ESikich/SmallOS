extern int dlopen_dep_magic(void);
extern int dlopen_dep_phase(void);
extern void dlopen_dep_mark_plugin_init(void);
extern void dlopen_dep_mark_plugin_fini(void);
extern int dlopen_dep_init_count(void);
extern int dlopen_dep_fini_count(void);

static int g_plugin_init_count;
static int g_plugin_fini_count;
static int g_plugin_state;
static int g_plugin_order_ok;

static void plugin_init(void) __attribute__((constructor));
static void plugin_fini(void) __attribute__((destructor));

static void plugin_init(void) {
    g_plugin_init_count++;
    if (dlopen_dep_magic() == 321 &&
        (dlopen_dep_phase() == 1 || dlopen_dep_phase() == 5)) {
        g_plugin_order_ok = 1;
    } else {
        g_plugin_order_ok = 0;
    }
    dlopen_dep_mark_plugin_init();
}

static void plugin_fini(void) {
    g_plugin_fini_count++;
    dlopen_dep_mark_plugin_fini();
}

int dlopen_plugin_value(void) {
    g_plugin_state++;
    return 4242 + g_plugin_state;
}

int dlopen_plugin_state(void) {
    return g_plugin_state;
}

int dlopen_plugin_init_count(void) {
    return g_plugin_init_count;
}

int dlopen_plugin_fini_count(void) {
    return g_plugin_fini_count;
}

int dlopen_plugin_order_ok(void) {
    return g_plugin_order_ok;
}

int dlopen_plugin_dep_init_count(void) {
    return dlopen_dep_init_count();
}

int dlopen_plugin_dep_fini_count(void) {
    return dlopen_dep_fini_count();
}
