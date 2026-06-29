#include "uapi_syscall.h"

typedef unsigned int u32;

static int s_init_seen;
static int s_touched;

static int dynfini_sc1(int num, u32 a) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num), "b"(a) : "memory");
    return ret;
}

static int dynfini_sc2(int num, u32 a, u32 b) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num), "b"(a), "c"(b) : "memory");
    return ret;
}

static int dynfini_sc3(int num, u32 a, u32 b, u32 c) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num), "b"(a), "c"(b), "d"(c) : "memory");
    return ret;
}

static void dynfini_init(void) __attribute__((constructor));
static void dynfini_fini(void) __attribute__((destructor));

static void dynfini_init(void) {
    s_init_seen = 1;
}

static void dynfini_fini(void) {
    const char msg[] = "dynfini fini\n";
    int fd;

    if (!s_touched) return;
    fd = dynfini_sc2(SYS_OPEN_MODE,
                     (u32)"dynfini.out",
                     SYS_OPEN_MODE_WRITE | SYS_OPEN_MODE_CREATE | SYS_OPEN_MODE_TRUNC);
    if (fd >= 0) {
        (void)dynfini_sc3(SYS_WRITEFD, (u32)fd, (u32)msg, sizeof(msg) - 1u);
        (void)dynfini_sc1(SYS_CLOSE, (u32)fd);
    }
}

int dynfini_init_seen(void) {
    return s_init_seen;
}

void dynfini_touch(void) {
    s_touched = 1;
}
