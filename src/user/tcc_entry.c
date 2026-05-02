#include "user_lib.h"
typedef struct TCCState TCCState;

TCCState* tcc_new(void);
int       tcc_parse_args(TCCState* s, int* argc, char*** argv);
int       tcc_set_output_type(TCCState* s, int output_type);
int       tcc_add_file(TCCState* s, const char* filename);
int       tcc_output_file(TCCState* s, const char* filename);
void      tcc_delete(TCCState* s);

static const char* find_output_name(int argc, char** argv) {
    for (int i = 0; i + 1 < argc; i++) {
        if (argv[i] && argv[i + 1] && argv[i][0] == '-' && argv[i][1] == 'o' && argv[i][2] == '\0') {
            return argv[i + 1];
        }
    }
    return 0;
}

static int tcc_start(int argc, char** argv) {
    int opt;
    TCCState* s;
    const char* out_name;

    s = tcc_new();
    if (!s) {
        return 111;
    }

    opt = tcc_parse_args(s, &argc, &argv);
    out_name = find_output_name(argc, argv);

    if (opt == 0) {
        tcc_set_output_type(s, 2);
        if (argc > 0 && argv && argv[argc - 1]) {
            tcc_add_file(s, argv[argc - 1]);
        }
        if (out_name) {
            tcc_output_file(s, out_name);
        }
    }

    tcc_delete(s);
    return opt;
}

void _start(int argc, char** argv) {
    sys_exit(tcc_start(argc, argv));
}
