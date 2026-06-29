static int g_dep_phase;
static int g_dep_init_count;
static int g_dep_fini_count;

static void dep_init(void) __attribute__((constructor));
static void dep_fini(void) __attribute__((destructor));

static void dep_init(void) {
    g_dep_init_count++;
    if (g_dep_phase == 0) {
        g_dep_phase = 1;
    } else if (g_dep_phase == 4) {
        g_dep_phase = 5;
    } else {
        g_dep_phase = 90;
    }
}

static void dep_fini(void) {
    g_dep_fini_count++;
    if (g_dep_phase == 3) {
        g_dep_phase = 4;
    } else {
        g_dep_phase = 91;
    }
}

int dlopen_dep_magic(void) {
    return 321;
}

int dlopen_dep_phase(void) {
    return g_dep_phase;
}

void dlopen_dep_mark_plugin_init(void) {
    if (g_dep_phase == 1) {
        g_dep_phase = 2;
    } else if (g_dep_phase == 5) {
        g_dep_phase = 6;
    } else {
        g_dep_phase = 92;
    }
}

void dlopen_dep_mark_plugin_fini(void) {
    if (g_dep_phase == 2) {
        g_dep_phase = 3;
    } else {
        g_dep_phase = 93;
    }
}

int dlopen_dep_init_count(void) {
    return g_dep_init_count;
}

int dlopen_dep_fini_count(void) {
    return g_dep_fini_count;
}
