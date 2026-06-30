#include "dlfcn.h"
#include "stdio.h"
#include "string.h"

typedef int (*abi_fn_t)(void);
typedef const char* (*name_fn_t)(void);
typedef int (*run_fn_t)(int);
typedef int (*count_fn_t)(void);

static int g_ok = 1;

static void expect_true(const char* name, int cond) {
    puts(cond ? name : "pluginhost check: FAIL");
    if (!cond) g_ok = 0;
}

static void* need_sym(void* handle, const char* symbol) {
    void* value = dlsym(handle, symbol);
    if (!value) {
        puts("pluginhost dlsym: FAIL");
        g_ok = 0;
    }
    return value;
}

int main(void) {
    void* alpha;
    void* beta;
    void* alpha2;
    abi_fn_t alpha_abi;
    abi_fn_t beta_abi;
    name_fn_t alpha_name;
    name_fn_t beta_name;
    run_fn_t alpha_run;
    run_fn_t beta_run;
    count_fn_t helper_init_count;
    count_fn_t helper_fini_count;

    puts("pluginhost start");

    alpha = dlopen("/usr/lib/smallos/plugins/alpha.so", RTLD_NOW | RTLD_GLOBAL);
    expect_true("pluginhost open-alpha: PASS", alpha != 0);
    beta = dlopen("/usr/lib/smallos/plugins/beta.so", RTLD_NOW | RTLD_GLOBAL);
    expect_true("pluginhost open-beta: PASS", beta != 0);

    alpha_abi = (abi_fn_t)need_sym(alpha, "smallos_plugin_abi");
    beta_abi = (abi_fn_t)need_sym(beta, "smallos_plugin_abi");
    alpha_name = (name_fn_t)need_sym(alpha, "smallos_plugin_name");
    beta_name = (name_fn_t)need_sym(beta, "smallos_plugin_name");
    alpha_run = (run_fn_t)need_sym(alpha, "smallos_plugin_run");
    beta_run = (run_fn_t)need_sym(beta, "smallos_plugin_run");
    helper_init_count = (count_fn_t)need_sym(RTLD_DEFAULT, "plugin_helper_init_count");
    helper_fini_count = (count_fn_t)need_sym(RTLD_DEFAULT, "plugin_helper_fini_count");

    if (alpha_abi && beta_abi && alpha_name && beta_name && alpha_run &&
        beta_run && helper_init_count && helper_fini_count) {
        expect_true("pluginhost abi: PASS", alpha_abi() == 1 && beta_abi() == 1);
        expect_true("pluginhost names: PASS",
                    strcmp(alpha_name(), "alpha") == 0 &&
                    strcmp(beta_name(), "beta") == 0);
        expect_true("pluginhost helper-shared: PASS",
                    helper_init_count() == 1 && helper_fini_count() == 0);
        expect_true("pluginhost run-alpha: PASS", alpha_run(5) == 106);
        expect_true("pluginhost run-beta: PASS", beta_run(6) == 207);
        expect_true("pluginhost state-alpha: PASS", alpha_run(5) == 107);
    }

    expect_true("pluginhost close-alpha: PASS", dlclose(alpha) == 0);
    expect_true("pluginhost stale-alpha: PASS",
                dlsym(alpha, "smallos_plugin_run") == 0 && dlerror() != 0);
    if (helper_fini_count) {
        expect_true("pluginhost helper-still-live: PASS", helper_fini_count() == 0);
    }
    expect_true("pluginhost close-beta: PASS", dlclose(beta) == 0);
    if (helper_fini_count) {
        expect_true("pluginhost helper-finalized: PASS", helper_fini_count() == 1);
    }

    alpha2 = dlopen("/usr/lib/smallos/plugins/alpha.so", RTLD_NOW);
    expect_true("pluginhost reopen-alpha: PASS", alpha2 != 0);
    if (alpha2) {
        alpha_run = (run_fn_t)need_sym(alpha2, "smallos_plugin_run");
        expect_true("pluginhost local-helper-hidden: PASS",
                    dlsym(RTLD_DEFAULT, "plugin_helper_init_count") == 0 &&
                        dlerror() != 0);
        helper_init_count = (count_fn_t)need_sym(alpha2, "plugin_helper_init_count");
        if (alpha_run && helper_init_count) {
            expect_true("pluginhost rerun-alpha: PASS", alpha_run(1) == 104);
            expect_true("pluginhost helper-reinit: PASS", helper_init_count() == 2);
        }
        expect_true("pluginhost close-reopen: PASS", dlclose(alpha2) == 0);
    }

    puts(g_ok ? "pluginhost PASS" : "pluginhost FAIL");
    return g_ok ? 0 : 1;
}
