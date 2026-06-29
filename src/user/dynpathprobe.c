#include "stdio.h"

extern int dynpath_value(void);

int main(void) {
    if (dynpath_value() == 4242) {
        puts("dynpathprobe: PASS");
        return 0;
    }
    puts("dynpathprobe: FAIL");
    return 1;
}
