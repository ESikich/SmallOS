#include "errno.h"
#include "fcntl.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "sys/stat.h"
#include "sys/wait.h"

static int failures = 0;

static void check(const char* name, int ok) {
    printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static int child_checks(void) {
    struct stat st;
    int fd;
    int local_failures = 0;

    if (setgid(1000) < 0 || setuid(1000) < 0) return 10;
    if (getuid() != 1000 || geteuid() != 1000 ||
        getgid() != 1000 || getegid() != 1000) {
        return 11;
    }

    errno = 0;
    if (!(open("/tmp/perm-root-private", O_RDONLY) < 0 && errno == EACCES)) {
        local_failures++;
    }
    errno = 0;
    if (!(access("/tmp/perm-root-private", R_OK) < 0 && errno == EACCES)) {
        local_failures++;
    }
    errno = 0;
    if (!(chmod("/tmp/perm-root-private", 0644) < 0 && errno == EPERM)) {
        local_failures++;
    }
    errno = 0;
    if (!(open("/tmp/perm-nowrite/child", O_CREAT | O_WRONLY, 0644) < 0 &&
          errno == EACCES)) {
        local_failures++;
    }

    fd = open("/tmp/perm-open/child", O_CREAT | O_RDWR, 0640);
    if (fd < 0) return 20;
    if (write(fd, "ok", 2) != 2) local_failures++;
    close(fd);

    if (stat("/tmp/perm-open/child", &st) < 0 ||
        st.st_uid != 1000 || st.st_gid != 1000 ||
        (st.st_mode & 0777) != 0640) {
        local_failures++;
    }
    if (access("/tmp/perm-open/child", W_OK) != 0) local_failures++;
    if (chmod("/tmp/perm-open/child", 0600) != 0) local_failures++;
    errno = 0;
    if (!(chown("/tmp/perm-open/child", 0, 0) < 0 && errno == EPERM)) {
        local_failures++;
    }
    return local_failures == 0 ? 0 : 30 + local_failures;
}

int main(void) {
    struct stat st;
    mode_t old_umask;
    int fd;
    int status = 0;
    pid_t pid;

    puts("permprobe start");

    unlink("/tmp/perm-umask");
    unlink("/tmp/perm-root-private");
    unlink("/tmp/perm-open/child");
    rmdir("/tmp/perm-open");
    unlink("/tmp/perm-nowrite/child");
    rmdir("/tmp/perm-nowrite");

    check("root ids", getuid() == 0 && geteuid() == 0 &&
                      getgid() == 0 && getegid() == 0);

    old_umask = umask(0077);
    fd = open("/tmp/perm-umask", O_CREAT | O_RDWR, 0666);
    check("umask old", old_umask == 0022);
    check("umask create open", fd >= 0);
    if (fd >= 0) close(fd);
    check("umask create mode", stat("/tmp/perm-umask", &st) == 0 &&
                               (st.st_mode & 0777) == 0600);
    (void)umask(old_umask);

    fd = open("/tmp/perm-root-private", O_CREAT | O_RDWR | O_TRUNC, 0600);
    check("root private create", fd >= 0);
    if (fd >= 0) {
        check("root private write", write(fd, "secret", 6) == 6);
        close(fd);
    }
    check("mkdir open dir", mkdir("/tmp/perm-open", 0777) == 0 &&
                            chmod("/tmp/perm-open", 0777) == 0);
    check("mkdir nowrite dir", mkdir("/tmp/perm-nowrite", 0555) == 0 &&
                               chmod("/tmp/perm-nowrite", 0555) == 0);

    pid = fork();
    if (pid < 0) {
        check("fork child", 0);
    } else if (pid == 0) {
        return child_checks();
    } else {
        check("wait child", waitpid(pid, &status, 0) == pid &&
                            WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }

    fd = open("/tmp/perm-root-private", O_RDONLY);
    check("root cleanup access", fd >= 0);
    if (fd >= 0) close(fd);
    unlink("/tmp/perm-open/child");
    rmdir("/tmp/perm-open");
    unlink("/tmp/perm-root-private");
    unlink("/tmp/perm-umask");
    rmdir("/tmp/perm-nowrite");

    printf("permprobe %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
