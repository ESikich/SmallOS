#include "errno.h"
#include "fcntl.h"
#include "signal.h"
#include "stdio.h"
#include "string.h"
#include "sys/mman.h"
#include "sys/signalfd.h"
#include "sys/timerfd.h"
#include "sys/wait.h"
#include "time.h"
#include "unistd.h"
#include "user_syscall.h"

#define MEASURED_LOOPS 5
#define PAGE_BYTES 4096u

static int failures;

static void check(const char* label, int ok) {
    printf("%s: %s\n", label, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static unsigned int positive_drift(unsigned int before, unsigned int after) {
    return before >= after ? before - after : 0u;
}

static unsigned int growth(unsigned int before, unsigned int after) {
    return after >= before ? after - before : 0u;
}

static void make_path(char* path, char suffix, int renamed) {
    const char* prefix = renamed ? "/tmp/lifetimeprobe-renamed-" :
                                  "/tmp/lifetimeprobe-";
    int i = 0;

    while (prefix[i]) {
        path[i] = prefix[i];
        i++;
    }
    path[i++] = suffix;
    path[i] = 0;
}

static int wait_for_child(pid_t pid, int expected_status) {
    int status = -1;

    if (waitpid(pid, &status, 0) != pid) {
        return 0;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == expected_status;
}

static int exercise_children(void) {
    pid_t pid;
    char* argv[] = {"/usr/bin/hello", 0};
    char* envp[] = {"PATH=/bin:/usr/bin", 0};

    pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        _exit(3);
    }
    if (!wait_for_child(pid, 3)) return 0;

    pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        int nullfd = open("/dev/null", O_WRONLY);

        if (nullfd >= 0) {
            (void)dup2(nullfd, 1);
            (void)dup2(nullfd, 2);
            if (nullfd > 2) close(nullfd);
        }
        execve("/usr/bin/hello", argv, envp);
        _exit(127);
    }
    return wait_for_child(pid, 0);
}

static int exercise_fds(void) {
    int fd = -1;
    int dupfd = -1;
    int cloexec = -1;
    char byte = 0;
    int ok = 1;

    fd = open("/usr/bin/hello", O_RDONLY);
    if (fd < 0) return 0;

    if (read(fd, &byte, 1) != 1) ok = 0;
    dupfd = dup(fd);
    if (dupfd < 0) ok = 0;
    if (ok && fcntl(dupfd, F_SETFD, FD_CLOEXEC) < 0) ok = 0;
    if (ok && (fcntl(dupfd, F_GETFD) & FD_CLOEXEC) == 0) ok = 0;

    cloexec = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    if (cloexec < 0) ok = 0;
    if (ok && (fcntl(cloexec, F_GETFD) & FD_CLOEXEC) == 0) ok = 0;

    if (cloexec >= 0 && close(cloexec) < 0) ok = 0;
    if (dupfd >= 0 && close(dupfd) < 0) ok = 0;
    if (close(fd) < 0) ok = 0;
    return ok;
}

static int exercise_pipe(void) {
    int fds[2];
    char buf[4] = {0};
    int ok = 1;

    if (pipe(fds) < 0) return 0;
    if (write(fds[1], "abc", 3) != 3) ok = 0;
    if (read(fds[0], buf, 3) != 3 || memcmp(buf, "abc", 3) != 0) ok = 0;
    if (close(fds[1]) < 0) ok = 0;
    if (read(fds[0], buf, 1) != 0) ok = 0;
    if (close(fds[0]) < 0) ok = 0;
    return ok;
}

static int exercise_special_fds(void) {
    struct itimerspec spec;
    sigset_t mask;
    int tfd = -1;
    int sfd = -1;
    int ok = 1;

    memset(&spec, 0, sizeof(spec));
    tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (tfd < 0) return 0;
    if (timerfd_settime(tfd, 0, &spec, 0) < 0) ok = 0;
    if (close(tfd) < 0) ok = 0;

    if (sigemptyset(&mask) < 0 || sigaddset(&mask, SIGUSR1) < 0) ok = 0;
    sfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (sfd < 0) ok = 0;
    if (sfd >= 0 && close(sfd) < 0) ok = 0;
    return ok;
}

static int exercise_file(char suffix) {
    char path[64];
    char renamed[64];
    char buf[16];
    int fd = -1;
    int ok = 1;

    make_path(path, suffix, 0);
    make_path(renamed, suffix, 1);
    (void)unlink(path);
    (void)unlink(renamed);

    fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) return 0;
    if (write(fd, "lifetime", 8) != 8) ok = 0;
    if (lseek(fd, 0, SEEK_SET) != 0) ok = 0;
    memset(buf, 0, sizeof(buf));
    if (read(fd, buf, 8) != 8 || memcmp(buf, "lifetime", 8) != 0) ok = 0;
    if (ftruncate(fd, 4) < 0) ok = 0;
    if (close(fd) < 0) ok = 0;
    fd = -1;

    if (rename(path, renamed) < 0) ok = 0;
    fd = open(renamed, O_RDONLY);
    if (fd < 0) {
        ok = 0;
    } else {
        memset(buf, 0, sizeof(buf));
        if (read(fd, buf, 8) != 4 || memcmp(buf, "life", 4) != 0) ok = 0;
        if (close(fd) < 0) ok = 0;
    }
    if (unlink(renamed) < 0) ok = 0;
    (void)unlink(path);
    (void)unlink(renamed);
    return ok;
}

static int exercise_mmap_cow(char suffix) {
    char* page;
    pid_t pid;
    int ok = 1;

    page = (char*)mmap(0, 3u * PAGE_BYTES, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) return 0;

    page[0] = suffix;
    page[PAGE_BYTES] = 'm';
    page[2u * PAGE_BYTES] = 'z';

    pid = fork();
    if (pid < 0) {
        ok = 0;
    } else if (pid == 0) {
        page[0] = 'c';
        page[PAGE_BYTES] = 'o';
        _exit(page[2u * PAGE_BYTES] == 'z' ? 0 : 4);
    } else {
        if (!wait_for_child(pid, 0)) ok = 0;
        if (page[0] != suffix || page[PAGE_BYTES] != 'm') ok = 0;
    }

    if (munmap(page, 3u * PAGE_BYTES) < 0) ok = 0;
    return ok;
}

static int workload_once(int cycle) {
    char suffix = (char)('a' + (cycle % 26));
    int ok = 1;

    if (!exercise_children()) ok = 0;
    if (!exercise_fds()) ok = 0;
    if (!exercise_pipe()) ok = 0;
    if (!exercise_special_fds()) ok = 0;
    if (!exercise_file(suffix)) ok = 0;
    if (!exercise_mmap_cow(suffix)) ok = 0;
    return ok;
}

int main(void) {
    sys_meminfo_t before;
    sys_meminfo_t after;
    unsigned int free_drift;
    unsigned int kalloc_drift;
    int workload_ok = 1;
    int process_ok;
    int fd_vm_ok;
    int pmm_ok;
    int kalloc_ok;

    puts("lifetimeprobe start");

    if (!workload_once(0)) {
        check("lifetimeprobe warmup", 0);
        puts("lifetimeprobe: FAIL");
        return 1;
    }
    check("lifetimeprobe warmup", 1);

    memset(&before, 0, sizeof(before));
    memset(&after, 0, sizeof(after));
    if (sys_meminfo(&before) < 0) {
        puts("lifetimeprobe meminfo before: FAIL");
        return 1;
    }

    for (int i = 0; i < MEASURED_LOOPS; i++) {
        if (!workload_once(i + 1)) {
            workload_ok = 0;
        }
    }

    if (sys_meminfo(&after) < 0) {
        puts("lifetimeprobe meminfo after: FAIL");
        return 1;
    }

    free_drift = positive_drift(before.pmm_free_frames, after.pmm_free_frames);
    kalloc_drift = growth(before.kalloc_used_bytes, after.kalloc_used_bytes);

    process_ok = after.process_count <= before.process_count + 1u &&
                 after.process_pages <= before.process_pages + 1u &&
                 after.kernel_stack_pages <= before.kernel_stack_pages + 8u;
    fd_vm_ok = after.fd_table_pages <= before.fd_table_pages + 1u &&
               after.vm_area_pages <= before.vm_area_pages + 1u &&
               after.ro_file_cache_mapped_refs <= before.ro_file_cache_mapped_refs &&
               after.pmm_refcounted_frames <= before.pmm_refcounted_frames &&
               after.pmm_shared_frames <= before.pmm_shared_frames;
    pmm_ok = free_drift <= 4u;
    kalloc_ok = kalloc_drift <= 2048u;

    check("lifetimeprobe workload", workload_ok);
    check("lifetimeprobe process release", process_ok);
    check("lifetimeprobe fd/vm release", fd_vm_ok);
    check("lifetimeprobe pmm drift", pmm_ok);
    check("lifetimeprobe kalloc drift", kalloc_ok);

    if (!failures) {
        puts("lifetimeprobe: PASS");
        return 0;
    }
    puts("lifetimeprobe: FAIL");
    return 1;
}
