#include "images.h"
#include "exec.h"
#include "terminal.h"

static int str_eq(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) {
            return 0;
        }
        i++;
    }
    return a[i] == '\0' && b[i] == '\0';
}

extern void img_hello_main(int argc, char** argv);
extern void img_args_main(int argc, char** argv);

typedef struct {
    const char* name;
    exec_entry_t entry;
} image_desc_t;

static image_desc_t images[] = {
    { "hello", img_hello_main },
    { "args",  img_args_main  },
};

#define IMAGE_COUNT (sizeof(images) / sizeof(images[0]))

void images_run(const char* name, int argc, char** argv) {
    for (unsigned int i = 0; i < IMAGE_COUNT; i++) {
        if (str_eq(name, images[i].name)) {
            if (!exec_run_entry(images[i].entry, argc, argv)) {
                terminal_puts("runimg: load failed\n");
            }
            return;
        }
    }

    terminal_puts("Image not found: ");
    terminal_puts(name);
    terminal_putc('\n');
}