static int g_helper_init_count;
static int g_helper_fini_count;

static void helper_init(void) __attribute__((constructor));
static void helper_fini(void) __attribute__((destructor));

static void helper_init(void) {
    g_helper_init_count++;
}

static void helper_fini(void) {
    g_helper_fini_count++;
}

int plugin_helper_add(int value, int amount) {
    return value + amount;
}

int plugin_helper_init_count(void) {
    return g_helper_init_count;
}

int plugin_helper_fini_count(void) {
    return g_helper_fini_count;
}
