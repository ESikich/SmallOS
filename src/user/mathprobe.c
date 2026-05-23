#include "math.h"
#include "stdio.h"

static int check_close(const char* label, double got, double want, double eps) {
    if (fabs(got - want) <= eps) {
        printf("mathprobe %s: PASS\n", label);
        return 1;
    }
    printf("mathprobe %s: FAIL got=%f want=%f\n", label, got, want);
    return 0;
}

int main(int argc, char** argv) {
    const double pi = 3.14159265358979323846;
    int ok = 1;

    (void)argc;
    (void)argv;

    puts("mathprobe start");
    ok &= check_close("sin0", sin(0.0), 0.0, 1.0e-9);
    ok &= check_close("cos0", cos(0.0), 1.0, 1.0e-9);
    ok &= check_close("sin_pi", sin(pi), 0.0, 1.0e-9);
    ok &= check_close("cos_pi", cos(pi), -1.0, 1.0e-9);
    ok &= check_close("sin_half_pi", sin(pi * 0.5), 1.0, 1.0e-9);
    ok &= check_close("cos_half_pi", cos(pi * 0.5), 0.0, 1.0e-9);
    ok &= check_close("sin_three_halves_pi", sin(pi * 1.5), -1.0, 1.0e-9);
    ok &= check_close("cos_two_pi", cos(pi * 2.0), 1.0, 1.0e-9);
    ok &= check_close("tan0", tan(0.0), 0.0, 1.0e-9);

    puts(ok ? "mathprobe PASS" : "mathprobe FAIL");
    return ok ? 0 : 1;
}
