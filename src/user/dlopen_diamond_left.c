extern void dlopen_diamond_mark(int bit);
extern int dlopen_diamond_mask(void);

static int g_left_init_count;
static int g_left_fini_count;

static void left_init(void) __attribute__((constructor));
static void left_fini(void) __attribute__((destructor));

static void left_init(void) {
    g_left_init_count++;
    if (dlopen_diamond_mask() & 1) {
        dlopen_diamond_mark(2);
    } else {
        dlopen_diamond_mark(0x1000);
    }
}

static void left_fini(void) {
    g_left_fini_count++;
    if (dlopen_diamond_mask() & 16) {
        dlopen_diamond_mark(32);
    } else {
        dlopen_diamond_mark(0x2000);
    }
}

int dlopen_diamond_left_value(void) {
    return 11;
}

int dlopen_diamond_left_init_count(void) {
    return g_left_init_count;
}

int dlopen_diamond_left_fini_count(void) {
    return g_left_fini_count;
}
