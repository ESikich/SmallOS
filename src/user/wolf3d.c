#include <unistd.h>

#include "wolf3d_port.h"

void wolf3d_upstream_main(void);

int main(int argc, char** argv) {
    wolf3d_argc = argc;
    wolf3d_argv = argv;
    (void)chdir("/usr/share/wolf3d");
    wolf3d_upstream_main();
    wolf3d_platform_shutdown();
    return 0;
}
