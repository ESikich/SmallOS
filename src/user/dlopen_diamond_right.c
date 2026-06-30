extern void dlopen_diamond_mark(int bit);
extern int dlopen_diamond_mask(void);

static int g_right_init_count;
static int g_right_fini_count;

static void right_init(void) __attribute__((constructor));
static void right_fini(void) __attribute__((destructor));

static void right_init(void) {
    g_right_init_count++;
    if (dlopen_diamond_mask() & 1) {
        dlopen_diamond_mark(4);
    } else {
        dlopen_diamond_mark(0x1000);
    }
}

static void right_fini(void) {
    g_right_fini_count++;
    if (dlopen_diamond_mask() & 16) {
        dlopen_diamond_mark(64);
    } else {
        dlopen_diamond_mark(0x2000);
    }
}

int dlopen_diamond_right_value(void) {
    return 17;
}

int dlopen_diamond_right_init_count(void) {
    return g_right_init_count;
}

int dlopen_diamond_right_fini_count(void) {
    return g_right_fini_count;
}
