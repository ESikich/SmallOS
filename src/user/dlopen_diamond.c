extern void dlopen_diamond_mark(int bit);
extern int dlopen_diamond_mask(void);
extern int dlopen_diamond_left_value(void);
extern int dlopen_diamond_right_value(void);
extern int dlopen_diamond_base_init_count(void);
extern int dlopen_diamond_base_fini_count(void);
extern int dlopen_diamond_left_init_count(void);
extern int dlopen_diamond_right_init_count(void);
extern int dlopen_diamond_left_fini_count(void);
extern int dlopen_diamond_right_fini_count(void);

static int g_top_init_count;
static int g_top_fini_count;
static int g_order_ok;

static void top_init(void) __attribute__((constructor));
static void top_fini(void) __attribute__((destructor));

static void top_init(void) {
    g_top_init_count++;
    if ((dlopen_diamond_mask() & 7) == 7) {
        g_order_ok = 1;
        dlopen_diamond_mark(8);
    } else {
        g_order_ok = 0;
        dlopen_diamond_mark(0x1000);
    }
}

static void top_fini(void) {
    g_top_fini_count++;
    if ((dlopen_diamond_mask() & 15) == 15) {
        dlopen_diamond_mark(16);
    } else {
        dlopen_diamond_mark(0x2000);
    }
}

int dlopen_diamond_value(void) {
    return dlopen_diamond_left_value() + dlopen_diamond_right_value();
}

int dlopen_diamond_order_ok(void) {
    return g_order_ok;
}

int dlopen_diamond_top_init_count(void) {
    return g_top_init_count;
}

int dlopen_diamond_top_fini_count(void) {
    return g_top_fini_count;
}

int dlopen_diamond_all_init_count(void) {
    return dlopen_diamond_base_init_count() +
           dlopen_diamond_left_init_count() +
           dlopen_diamond_right_init_count() +
           g_top_init_count;
}

int dlopen_diamond_all_fini_count(void) {
    return dlopen_diamond_base_fini_count() +
           dlopen_diamond_left_fini_count() +
           dlopen_diamond_right_fini_count() +
           g_top_fini_count;
}
