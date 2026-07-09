#include "fcntl.h"
#include "stdio.h"
#include "stdlib.h"
#include "sys/mman.h"
#include "sys/socket.h"
#include "sys/wait.h"
#include "unistd.h"
#include "user_syscall.h"

#define LOOPS 6

static int failures = 0;

static void passfail(const char* label, int ok) {
    printf("%s: %s\n", label, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static void workload_once(void) {
    int status = 0;
    int fds[2];
    int fd;
    int s;
    char byte;
    char* page;
    int pid = fork();

    if (pid == 0) _exit(0);
    if (pid > 0) {
        (void)waitpid(pid, &status, 0);
    } else {
        failures++;
    }

    if (pipe(fds) == 0) {
        (void)write(fds[1], "x", 1);
        (void)read(fds[0], &byte, 1);
        close(fds[0]);
        close(fds[1]);
    } else {
        failures++;
    }

    fd = open("usr/bin/hello", O_RDONLY);
    if (fd >= 0) {
        (void)read(fd, &byte, 1);
        close(fd);
    } else {
        failures++;
    }

    page = (char*)mmap(0, 2u * 4096u, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page != MAP_FAILED) {
        page[0] = 'm';
        page[4096] = 'n';
        (void)munmap(page, 2u * 4096u);
    } else {
        failures++;
    }

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s >= 0) {
        close(s);
    } else {
        failures++;
    }
}

int main(void) {
    sys_meminfo_t before = {0};
    sys_meminfo_t after = {0};
    unsigned int free_drift;
    unsigned int kalloc_drift;

    puts("memleakprobe start");
    workload_once(); /* warm persistent caches/tables before measuring */

    if (sys_meminfo(&before) < 0) {
        puts("memleakprobe meminfo before: FAIL");
        return 1;
    }

    for (int i = 0; i < LOOPS; i++) {
        workload_once();
    }

    if (sys_meminfo(&after) < 0) {
        puts("memleakprobe meminfo after: FAIL");
        return 1;
    }

    free_drift = before.pmm_free_frames >= after.pmm_free_frames
               ? before.pmm_free_frames - after.pmm_free_frames
               : 0u;
    kalloc_drift = after.kalloc_used_bytes >= before.kalloc_used_bytes
                 ? after.kalloc_used_bytes - before.kalloc_used_bytes
                 : 0u;

    passfail("memleakprobe workload", failures == 0);
    passfail("memleakprobe pmm drift", free_drift <= 2u);
    passfail("memleakprobe kalloc drift", kalloc_drift <= 1024u);

    if (!failures) {
        puts("memleakprobe: PASS");
        return 0;
    }
    puts("memleakprobe: FAIL");
    return 1;
}
