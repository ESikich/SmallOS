#include "stdio.h"
#include "stdlib.h"
#include "fcntl.h"
#include "string.h"
#include "sys/mman.h"
#include "sys/wait.h"
#include "unistd.h"
#include "user_syscall.h"

#define BIG_PAGES 12u
#define BIG_SIZE  (BIG_PAGES * 4096u)

static int g_value = 11;

static int failures = 0;

static void passfail(const char* label, int ok) {
    printf("%s: %s\n", label, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static int child_isolation(char* heap, char* anon, volatile char* stack_ptr) {
    g_value = 42;
    heap[0] = 'c';
    heap[4096] = 'd';
    anon[0] = 'm';
    anon[4096] = 'n';
    *stack_ptr = 's';
    return (g_value == 42 &&
            heap[0] == 'c' && heap[4096] == 'd' &&
            anon[0] == 'm' && anon[4096] == 'n' && anon[8192] == 'C' &&
            *stack_ptr == 's') ? 7 : 2;
}

static void demand_mmap_check(void) {
    sys_meminfo_t before = {0};
    sys_meminfo_t after_map = {0};
    sys_meminfo_t after_touch = {0};
    char* lazy;
    unsigned int reserve_drop;
    unsigned int touch_drop;

    if (sys_meminfo(&before) < 0) {
        puts("cowprobe demand before: FAIL");
        failures++;
        return;
    }
    lazy = (char*)mmap(0, 8u * 4096u, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (lazy == MAP_FAILED) {
        puts("cowprobe demand mmap: FAIL");
        failures++;
        return;
    }
    if (sys_meminfo(&after_map) < 0) {
        puts("cowprobe demand mapped: FAIL");
        failures++;
        munmap(lazy, 8u * 4096u);
        return;
    }
    lazy[0] = 'L';
    if (sys_meminfo(&after_touch) < 0) {
        puts("cowprobe demand touched: FAIL");
        failures++;
        munmap(lazy, 8u * 4096u);
        return;
    }

    reserve_drop = before.pmm_free_frames >= after_map.pmm_free_frames ?
                   before.pmm_free_frames - after_map.pmm_free_frames : 0u;
    touch_drop = after_map.pmm_free_frames >= after_touch.pmm_free_frames ?
                 after_map.pmm_free_frames - after_touch.pmm_free_frames : 0u;
    passfail("cowprobe demand reserve", reserve_drop < 3u);
    passfail("cowprobe demand touch", touch_drop >= 1u && lazy[0] == 'L');
    munmap(lazy, 8u * 4096u);
}

static void fork_chain_check(void) {
    int original = g_value;
    int status = 0;
    pid_t child = fork();

    if (child < 0) {
        puts("cowprobe fork chain: FAIL");
        failures++;
        return;
    }
    if (child == 0) {
        pid_t grand;
        int grand_status = 0;
        g_value = 70;
        grand = fork();
        if (grand < 0) exit(4);
        if (grand == 0) {
            g_value = 90;
            exit(g_value == 90 ? 9 : 5);
        }
        if (waitpid(grand, &grand_status, 0) != grand ||
            grand_status != (9 << 8) ||
            g_value != 70) {
            exit(6);
        }
        exit(8);
    }

    if (waitpid(child, &status, 0) == child &&
        status == (8 << 8) &&
        g_value == original) {
        puts("cowprobe fork chain: PASS");
    } else {
        puts("cowprobe fork chain: FAIL");
        failures++;
    }
}

static void file_private_check(void) {
    const char* path = "/tmp/cowmap.txt";
    char original[8] = {0};
    int fd;
    char* page;

    if (sys_writefile_path(path, "ABCD", 4) < 0) {
        puts("cowprobe file setup: FAIL");
        failures++;
        return;
    }
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        puts("cowprobe file open: FAIL");
        failures++;
        return;
    }
    page = (char*)mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    close(fd);
    if (page == MAP_FAILED) {
        puts("cowprobe file mmap: FAIL");
        failures++;
        return;
    }
    if (page[0] != 'A' || page[1] != 'B') {
        puts("cowprobe file read: FAIL");
        failures++;
        munmap(page, 4096);
        return;
    }
    page[0] = 'Z';
    fd = open(path, O_RDONLY);
    if (fd >= 0) {
        (void)read(fd, original, 4);
        close(fd);
    }
    passfail("cowprobe file private", page[0] == 'Z' && original[0] == 'A');
    munmap(page, 4096);
}

static void munmap_split_check(void) {
    char* area = (char*)mmap(0, 3u * 4096u, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    char* middle;
    int ok = 0;

    if (area == MAP_FAILED) {
        puts("cowprobe munmap split: FAIL");
        failures++;
        return;
    }

    area[0] = 'A';
    area[4096] = 'B';
    area[8192] = 'C';

    if (munmap(area + 4096, 4096) == 0) {
        middle = (char*)mmap(area + 4096, 4096, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (middle == area + 4096) {
            middle[0] = 'M';
            ok = area[0] == 'A' && middle[0] == 'M' && area[8192] == 'C';
        }
    }

    passfail("cowprobe munmap split", ok);
    (void)munmap(area, 4096);
    (void)munmap(area + 4096, 4096);
    (void)munmap(area + 8192, 4096);
}

static void mprotect_fork_check(void) {
    char* page = (char*)mmap(0, 4096, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    pid_t child;
    int status = 0;
    int ok = 0;

    if (page == MAP_FAILED) {
        puts("cowprobe mprotect fork: FAIL");
        failures++;
        return;
    }

    page[0] = 'R';
    if (mprotect(page, 4096, PROT_READ) != 0) {
        puts("cowprobe mprotect fork: FAIL");
        failures++;
        munmap(page, 4096);
        return;
    }

    child = fork();
    if (child < 0) {
        puts("cowprobe mprotect fork: FAIL");
        failures++;
        munmap(page, 4096);
        return;
    }
    if (child == 0) {
        if (mprotect(page, 4096, PROT_READ | PROT_WRITE) != 0) {
            exit(5);
        }
        page[0] = 'C';
        exit(page[0] == 'C' ? 12 : 6);
    }

    if (waitpid(child, &status, 0) == child &&
        status == (12 << 8) &&
        page[0] == 'R' &&
        mprotect(page, 4096, PROT_READ | PROT_WRITE) == 0) {
        page[0] = 'P';
        ok = page[0] == 'P';
    }

    passfail("cowprobe mprotect fork", ok);
    munmap(page, 4096);
}

int main(void) {
    char* heap;
    char* anon;
    volatile char stack_byte = 'p';
    sys_meminfo_t before = {0};
    sys_meminfo_t after_fork = {0};
    sys_meminfo_t after_wait = {0};
    pid_t pid;
    int status = 0;
    int sync_pipe[2];
    char token = 'x';
    unsigned int fork_frame_drop;

    demand_mmap_check();
    fork_chain_check();
    file_private_check();
    munmap_split_check();
    mprotect_fork_check();

    heap = (char*)malloc(BIG_SIZE);
    if (!heap) {
        puts("cowprobe malloc: FAIL");
        return 1;
    }
    for (unsigned int i = 0; i < BIG_SIZE; i += 4096u) {
        heap[i] = (char)('a' + (i / 4096u) % 20u);
    }

    anon = (char*)mmap(0, BIG_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (anon == MAP_FAILED) {
        puts("cowprobe mmap: FAIL");
        free(heap);
        return 1;
    }
    for (unsigned int i = 0; i < BIG_SIZE; i += 4096u) {
        anon[i] = (char)('A' + (i / 4096u) % 20u);
    }

    if (pipe(sync_pipe) < 0) {
        puts("cowprobe pipe: FAIL");
        munmap(anon, BIG_SIZE);
        free(heap);
        return 1;
    }

    if (sys_meminfo(&before) < 0) {
        puts("cowprobe meminfo before: FAIL");
        close(sync_pipe[0]);
        close(sync_pipe[1]);
        munmap(anon, BIG_SIZE);
        free(heap);
        return 1;
    }

    pid = fork();
    if (pid < 0) {
        puts("cowprobe fork: FAIL");
        close(sync_pipe[0]);
        close(sync_pipe[1]);
        munmap(anon, BIG_SIZE);
        free(heap);
        return 1;
    }
    if (pid == 0) {
        close(sync_pipe[1]);
        if (read(sync_pipe[0], &token, 1) != 1) {
            return 6;
        }
        close(sync_pipe[0]);
        return child_isolation(heap, anon, &stack_byte);
    }

    close(sync_pipe[0]);

    if (sys_meminfo(&after_fork) < 0) {
        puts("cowprobe meminfo after fork: FAIL");
        failures++;
    }

    fork_frame_drop = before.pmm_free_frames >= after_fork.pmm_free_frames ?
                      before.pmm_free_frames - after_fork.pmm_free_frames : 0u;
    passfail("cowprobe fork frame drop", fork_frame_drop < BIG_PAGES + 10u);

    passfail("cowprobe parent globals", g_value == 11);
    passfail("cowprobe parent heap", heap[0] != 'c' && heap[4096] != 'd');
    passfail("cowprobe parent mmap", anon[0] != 'm' && anon[4096] != 'n');
    passfail("cowprobe parent stack", stack_byte == 'p');

    heap[0] = 'P';
    anon[0] = 'Q';
    stack_byte = 'R';
    passfail("cowprobe parent writes", heap[0] == 'P' && anon[0] == 'Q' && stack_byte == 'R');

    if (mprotect(anon + 8192, 4096, PROT_READ) == 0 &&
        mprotect(anon + 8192, 4096, PROT_READ | PROT_WRITE) == 0) {
        anon[8192] = 'Z';
        passfail("cowprobe mprotect write", anon[8192] == 'Z');
    } else {
        puts("cowprobe mprotect write: FAIL");
        failures++;
    }

    if (write(sync_pipe[1], &token, 1) != 1) {
        puts("cowprobe release child: FAIL");
        failures++;
    }
    close(sync_pipe[1]);

    if (waitpid(pid, &status, 0) != pid || status != (7 << 8)) {
        puts("cowprobe wait: FAIL");
        failures++;
    } else {
        puts("cowprobe wait: PASS");
    }

    if (sys_meminfo(&after_wait) == 0) {
        passfail("cowprobe child frames released",
                 after_wait.pmm_free_frames >= after_fork.pmm_free_frames);
    } else {
        puts("cowprobe meminfo after wait: FAIL");
        failures++;
    }

    munmap(anon, BIG_SIZE);
    free(heap);

    if (failures == 0) {
        puts("cowprobe: PASS");
        return 0;
    }
    puts("cowprobe: FAIL");
    return 1;
}
