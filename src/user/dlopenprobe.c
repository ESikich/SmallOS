#include "dlfcn.h"
#include "stdio.h"
#include "string.h"

typedef int (*dl_int_fn)(void);

static int g_ok = 1;

static void expect_true(const char* name, int cond) {
    puts(cond ? name : "dlopenprobe check: FAIL");
    if (!cond) g_ok = 0;
}

static dl_int_fn need_sym(void* handle, const char* name) {
    dl_int_fn fn = (dl_int_fn)dlsym(handle, name);
    if (!fn) {
        puts("dlopenprobe dlsym: FAIL");
        g_ok = 0;
    }
    return fn;
}

int main(int argc, char** argv, char** envp) {
    void* h1;
    void* h2;
    void* h3;
    void* main_handle;
    const char* err;
    dl_int_fn value_fn;
    dl_int_fn state_fn;
    dl_int_fn init_count_fn;
    dl_int_fn fini_count_fn;
    dl_int_fn order_ok_fn;
    dl_int_fn dep_phase_fn;
    dl_int_fn dep_init_count_fn;
    dl_int_fn dep_fini_count_fn;
    dl_int_fn diamond_value_fn;
    dl_int_fn diamond_mask_fn;
    dl_int_fn diamond_order_ok_fn;
    dl_int_fn diamond_init_count_fn;
    dl_int_fn diamond_fini_count_fn;
    void* pinned_libc;
    void* dup_abs;
    void* dup_soname;
    void* diamond1;
    void* diamond2;

    (void)argc;
    (void)argv;
    (void)envp;

    puts("dlopenprobe start");

    main_handle = dlopen(0, RTLD_NOW);
    expect_true("dlopenprobe main-handle: PASS", main_handle != 0);
    expect_true("dlopenprobe main-symbol: PASS", dlsym(RTLD_DEFAULT, "dlopen") != 0);
    expect_true("dlopenprobe invalid-flags: PASS",
                dlopen("/usr/lib/libdlplug.so", RTLD_NOW | 0x8000) == 0 &&
                    dlerror() != 0);
    expect_true("dlopenprobe bad-token: PASS",
                dlopen("$ORIGIN/libdlplug.so", RTLD_NOW) == 0 &&
                    dlerror() != 0);
    expect_true("dlopenprobe invalid-close: PASS",
                dlclose((void*)0x12345678) < 0 && dlerror() != 0);

    pinned_libc = dlopen("/lib/libc.so", RTLD_NOW);
    expect_true("dlopenprobe pinned-libc: PASS",
                pinned_libc != 0 &&
                    dlsym(pinned_libc, "dlopen") != 0 &&
                    dlclose(pinned_libc) == 0);

    dup_abs = dlopen("/lib/libdynfini.so", RTLD_NOW);
    dup_soname = dlopen("libdynfini.so", RTLD_NOW);
    expect_true("dlopenprobe duplicate-soname: PASS",
                dup_abs != 0 && dup_abs == dup_soname);
    if (dup_abs) {
        expect_true("dlopenprobe duplicate-close1: PASS", dlclose(dup_abs) == 0);
    }
    if (dup_soname) {
        expect_true("dlopenprobe duplicate-close2: PASS", dlclose(dup_soname) == 0);
    }

    h1 = dlopen("/usr/lib/libdoesnotexist.so", RTLD_NOW);
    err = dlerror();
    expect_true("dlopenprobe missing-lib: PASS",
                h1 == 0 && err && strstr(err, "library not found") != 0);
    expect_true("dlopenprobe dlerror-clear: PASS", dlerror() == 0);

    h1 = dlopen("/usr/lib/libdlplug.so", RTLD_NOW | RTLD_GLOBAL);
    expect_true("dlopenprobe open1: PASS", h1 != 0);
    h2 = dlopen("/usr/lib/libdlplug.so", RTLD_LAZY);
    expect_true("dlopenprobe open2: PASS", h2 != 0);

    expect_true("dlopenprobe missing-symbol: PASS",
                dlsym(h1, "dlopen_plugin_missing") == 0 &&
                    dlerror() != 0);

    value_fn = need_sym(h1, "dlopen_plugin_value");
    state_fn = need_sym(h1, "dlopen_plugin_state");
    init_count_fn = need_sym(h1, "dlopen_plugin_init_count");
    fini_count_fn = need_sym(h1, "dlopen_plugin_fini_count");
    order_ok_fn = need_sym(h1, "dlopen_plugin_order_ok");
    dep_phase_fn = need_sym(h1, "dlopen_dep_phase");
    dep_init_count_fn = need_sym(h1, "dlopen_plugin_dep_init_count");
    dep_fini_count_fn = need_sym(h1, "dlopen_plugin_dep_fini_count");

    if (value_fn && state_fn && init_count_fn && fini_count_fn &&
        order_ok_fn && dep_phase_fn && dep_init_count_fn && dep_fini_count_fn) {
        expect_true("dlopenprobe call: PASS", value_fn() == 4243);
        expect_true("dlopenprobe state: PASS", state_fn() == 1);
        expect_true("dlopenprobe init-order: PASS",
                    init_count_fn() == 1 &&
                    dep_init_count_fn() == 1 &&
                    order_ok_fn() == 1 &&
                    dep_phase_fn() == 2);
        expect_true("dlopenprobe close-first: PASS",
                    dlclose(h1) == 0 && fini_count_fn() == 0);
        expect_true("dlopenprobe close-last: PASS", dlclose(h2) == 0);
        expect_true("dlopenprobe stale-handle: PASS",
                    dlsym(h1, "dlopen_plugin_value") == 0 && dlerror() != 0);
    }

    h3 = dlopen("/usr/lib/libdlplug.so", RTLD_NOW);
    expect_true("dlopenprobe reopen: PASS", h3 != 0);
    if (h3) {
        init_count_fn = need_sym(h3, "dlopen_plugin_init_count");
        fini_count_fn = need_sym(h3, "dlopen_plugin_fini_count");
        dep_phase_fn = need_sym(h3, "dlopen_dep_phase");
        dep_init_count_fn = need_sym(h3, "dlopen_plugin_dep_init_count");
        dep_fini_count_fn = need_sym(h3, "dlopen_plugin_dep_fini_count");
        if (init_count_fn && fini_count_fn && dep_phase_fn &&
            dep_init_count_fn && dep_fini_count_fn) {
            expect_true("dlopenprobe refcount-fini: PASS",
                        init_count_fn() == 2 &&
                        fini_count_fn() == 1 &&
                        dep_init_count_fn() == 2 &&
                        dep_fini_count_fn() == 1 &&
                        dep_phase_fn() == 6);
        }
        expect_true("dlopenprobe close-reopen: PASS", dlclose(h3) == 0);
    }

    diamond1 = dlopen("/usr/lib/libdldiamond.so", RTLD_NOW | RTLD_GLOBAL);
    diamond2 = dlopen("libdldiamond.so", RTLD_NOW);
    expect_true("dlopenprobe diamond-open: PASS",
                diamond1 != 0 && diamond1 == diamond2);
    if (diamond1 && diamond2) {
        diamond_value_fn = need_sym(diamond1, "dlopen_diamond_value");
        diamond_mask_fn = need_sym(diamond1, "dlopen_diamond_mask");
        diamond_order_ok_fn = need_sym(diamond1, "dlopen_diamond_order_ok");
        diamond_init_count_fn = need_sym(diamond1, "dlopen_diamond_all_init_count");
        diamond_fini_count_fn = need_sym(diamond1, "dlopen_diamond_all_fini_count");
        if (diamond_value_fn && diamond_mask_fn && diamond_order_ok_fn &&
            diamond_init_count_fn && diamond_fini_count_fn) {
            expect_true("dlopenprobe diamond-call: PASS",
                        diamond_value_fn() == 28 &&
                        diamond_order_ok_fn() == 1 &&
                        diamond_mask_fn() == 15 &&
                        diamond_init_count_fn() == 4);
            expect_true("dlopenprobe diamond-close1: PASS",
                        dlclose(diamond1) == 0 &&
                        diamond_fini_count_fn() == 0);
            expect_true("dlopenprobe diamond-close2: PASS",
                        dlclose(diamond2) == 0 &&
                        diamond_fini_count_fn() == 4 &&
                        diamond_mask_fn() == 255);
        }
    }

    puts(g_ok ? "dlopenprobe PASS" : "dlopenprobe FAIL");
    return g_ok ? 0 : 1;
}
