#include "stdio.h"
#include "sys/wait.h"
#include "unistd.h"
#include "user_syscall.h"

#define CHILDREN 40

static int pid_seen(int* pids, int count, int pid) {
    for (int i = 0; i < count; i++) {
        if (pids[i] == pid) return 1;
    }
    return 0;
}

int main(void) {
    sys_meminfo_t before = {0};
    sys_meminfo_t during = {0};
    sys_meminfo_t after = {0};
    int pids[CHILDREN] = {0};
    int failures = 0;

    puts("procscaleprobe start");
    if (sys_meminfo(&before) < 0) {
        puts("procscaleprobe meminfo before: FAIL");
        return 1;
    }

    for (int i = 0; i < CHILDREN; i++) {
        int pid = fork();
        if (pid < 0) {
            puts("procscaleprobe fork: FAIL");
            failures++;
            break;
        }
        if (pid == 0) {
            sleep(1);
            return 0;
        }
        if (pid_seen(pids, i, pid)) {
            puts("procscaleprobe unique pids: FAIL");
            failures++;
        }
        pids[i] = pid;
    }

    if (sys_meminfo(&during) == 0 &&
        during.process_count >= before.process_count + CHILDREN) {
        puts("procscaleprobe process count: PASS");
    } else {
        puts("procscaleprobe process count: FAIL");
        failures++;
    }

    for (int i = 0; i < CHILDREN; i++) {
        int status = 0;
        if (pids[i] <= 0) continue;
        if (waitpid(pids[i], &status, 0) != pids[i] || status != 0) {
            puts("procscaleprobe wait: FAIL");
            failures++;
        }
    }
    if (!failures) puts("procscaleprobe wait: PASS");

    if (sys_meminfo(&after) == 0 &&
        after.process_count <= before.process_count + 1u &&
        after.pmm_free_frames + 4u >= before.pmm_free_frames) {
        puts("procscaleprobe release: PASS");
    } else {
        puts("procscaleprobe release: FAIL");
        failures++;
    }

    if (!failures) {
        puts("procscaleprobe: PASS");
        return 0;
    }
    puts("procscaleprobe: FAIL");
    return 1;
}
