#include "errno.h"
#include "fcntl.h"
#include "math.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "sys/mman.h"
#include "sys/stat.h"
#include "unistd.h"
#include "user_syscall.h"

extern int dynfini_init_seen(void);
extern void dynfini_touch(void);

static int check_file_mmap(void) {
    sys_meminfo_t before = {0};
    sys_meminfo_t mapped = {0};
    sys_meminfo_t after = {0};
    void* page;
    void* bad;
    int fd;
    int ok = 1;

    fd = open("/lib/libc.so", O_RDONLY);
    if (fd < 0) {
        puts("dynlinkprobe mmap-open: FAIL");
        return 0;
    }

    if (sys_meminfo(&before) < 0) {
        puts("dynlinkprobe mmap-before: FAIL");
        ok = 0;
    }

    errno = 0;
    page = mmap(0, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    if (page == MAP_FAILED) {
        puts("dynlinkprobe mmap-readonly: FAIL");
        close(fd);
        return 0;
    }
    if (((unsigned char*)page)[0] == 0x7f &&
        ((unsigned char*)page)[1] == 'E' &&
        ((unsigned char*)page)[2] == 'L' &&
        ((unsigned char*)page)[3] == 'F') {
        puts("dynlinkprobe mmap-readonly: PASS");
    } else {
        puts("dynlinkprobe mmap-readonly: FAIL");
        ok = 0;
    }

    if (sys_meminfo(&mapped) == 0 &&
        mapped.ro_file_cache_pages >= before.ro_file_cache_pages &&
        mapped.ro_file_cache_mapped_refs > before.ro_file_cache_mapped_refs) {
        puts("dynlinkprobe mmap-ref: PASS");
    } else {
        puts("dynlinkprobe mmap-ref: FAIL");
        ok = 0;
    }

    errno = 0;
    if (mprotect(page, 4096, PROT_READ | PROT_WRITE) < 0 && errno == ENOSYS) {
        puts("dynlinkprobe mmap-mprotect-write: PASS");
    } else {
        puts("dynlinkprobe mmap-mprotect-write: FAIL");
        ok = 0;
    }

    if (munmap(page, 4096) == 0 &&
        sys_meminfo(&after) == 0 &&
        after.ro_file_cache_mapped_refs < mapped.ro_file_cache_mapped_refs) {
        puts("dynlinkprobe mmap-unmap-ref: PASS");
    } else {
        puts("dynlinkprobe mmap-unmap-ref: FAIL");
        ok = 0;
    }

    errno = 0;
    bad = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (bad == MAP_FAILED && errno == ENOSYS) {
        puts("dynlinkprobe mmap-write-reject: PASS");
    } else {
        puts("dynlinkprobe mmap-write-reject: FAIL");
        if (bad != MAP_FAILED) munmap(bad, 4096);
        ok = 0;
    }

    errno = 0;
    bad = mmap(0, 4096, PROT_READ, MAP_PRIVATE, fd, 1);
    if (bad == MAP_FAILED && errno == EINVAL) {
        puts("dynlinkprobe mmap-offset-reject: PASS");
    } else {
        puts("dynlinkprobe mmap-offset-reject: FAIL");
        if (bad != MAP_FAILED) munmap(bad, 4096);
        ok = 0;
    }

    close(fd);
    return ok;
}

int main(int argc, char** argv, char** envp) {
    char cwd[128];
    struct stat st;
    char* p;
    int ok = 1;

    (void)argc;
    (void)argv;

    puts("dynlinkprobe start");

    p = (char*)malloc(32);
    if (!p) {
        puts("dynlinkprobe malloc: FAIL");
        ok = 0;
    } else {
        strcpy(p, "dynamic malloc ok");
        puts(strcmp(p, "dynamic malloc ok") == 0 ? "dynlinkprobe malloc: PASS"
                                                 : "dynlinkprobe malloc: FAIL");
        free(p);
    }

    if (getcwd(cwd, sizeof(cwd)) && cwd[0] == '/') {
        puts("dynlinkprobe getcwd: PASS");
    } else {
        puts("dynlinkprobe getcwd: FAIL");
        ok = 0;
    }

    if (stat("/lib/libc.so", &st) == 0 && st.st_size > 0) {
        puts("dynlinkprobe stat: PASS");
    } else {
        puts("dynlinkprobe stat: FAIL");
        ok = 0;
    }

    if (sqrt(81.0) == 9.0) {
        puts("dynlinkprobe math: PASS");
    } else {
        puts("dynlinkprobe math: FAIL");
        ok = 0;
    }

    if (envp && getenv("PATH")) {
        puts("dynlinkprobe env: PASS");
    } else {
        puts("dynlinkprobe env: FAIL");
        ok = 0;
    }

    if (dynfini_init_seen()) {
        puts("dynlinkprobe fini-init: PASS");
    } else {
        puts("dynlinkprobe fini-init: FAIL");
        ok = 0;
    }
    dynfini_touch();

    if (!check_file_mmap()) ok = 0;

    {
        sys_meminfo_t before;
        sys_meminfo_t during;
        int status = 0;
        char* child_argv[] = { "usr/libexec/tests/sleep_test", 0 };
        int pid;

        if (sys_meminfo(&before) == 0 &&
            before.ro_file_cache_pages > 0 &&
            before.ro_file_cache_mapped_refs > 0) {
            puts("dynlinkprobe shared-cache: PASS");
        } else {
            puts("dynlinkprobe shared-cache: FAIL");
            ok = 0;
        }

        pid = sys_exec("usr/libexec/tests/sleep_test", 1, child_argv);
        if (pid > 0) {
            int shared = 0;
            for (int i = 0; i < 32; i++) {
                sys_sleep(1);
                if (sys_meminfo(&during) == 0 &&
                    during.ro_file_cache_mapped_refs > before.ro_file_cache_mapped_refs) {
                    shared = 1;
                    break;
                }
            }
            if (shared) {
                puts("dynlinkprobe shared-child: PASS");
            } else {
                puts("dynlinkprobe shared-child: FAIL");
                ok = 0;
            }
            if (sys_waitpid(pid, &status, 0) < 0 || status != 0) {
                puts("dynlinkprobe shared-child-wait: FAIL");
                ok = 0;
            }
        } else {
            puts("dynlinkprobe shared-child-launch: FAIL");
            ok = 0;
        }
    }

    {
        sys_meminfo_t before;
        sys_meminfo_t child;
        int status = 0;
        int pid;

        if (sys_meminfo(&before) < 0) {
            puts("dynlinkprobe fork-cache-before: FAIL");
            ok = 0;
        } else {
            pid = fork();
            if (pid < 0) {
                puts("dynlinkprobe fork-cache-fork: FAIL");
                ok = 0;
            } else if (pid == 0) {
                if (sys_meminfo(&child) == 0 &&
                    child.ro_file_cache_pages == before.ro_file_cache_pages &&
                    child.ro_file_cache_mapped_refs >= before.ro_file_cache_mapped_refs) {
                    return 23;
                }
                return 24;
            } else if (sys_waitpid(pid, &status, 0) < 0 || status != (23 << 8)) {
                puts("dynlinkprobe fork-cache: FAIL");
                ok = 0;
            } else {
                puts("dynlinkprobe fork-cache: PASS");
            }
        }
    }

    puts(ok ? "dynlinkprobe PASS" : "dynlinkprobe FAIL");
    return ok ? 0 : 1;
}
