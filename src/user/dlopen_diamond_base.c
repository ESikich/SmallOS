static int g_mask;
static int g_init_count;
static int g_fini_count;

static void base_init(void) __attribute__((constructor));
static void base_fini(void) __attribute__((destructor));

static void base_init(void) {
    g_init_count++;
    if (g_mask == 0) {
        g_mask = 1;
    } else if (g_mask == 255) {
        g_mask = 1;
    } else {
        g_mask = 0x4000;
    }
}

static void base_fini(void) {
    g_fini_count++;
    if ((g_mask & 0x7f) == 0x7f) {
        g_mask |= 0x80;
    } else {
        g_mask = 0x8000;
    }
}

void dlopen_diamond_mark(int bit) {
    g_mask |= bit;
}

int dlopen_diamond_mask(void) {
    return g_mask;
}

int dlopen_diamond_base_init_count(void) {
    return g_init_count;
}

int dlopen_diamond_base_fini_count(void) {
    return g_fini_count;
}
