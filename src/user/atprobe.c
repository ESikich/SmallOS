#include "errno.h"
#include "fcntl.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "sys/stat.h"

static int failures = 0;

static void check(const char* name, int ok) {
    printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static void cleanup(void) {
    unlink("/tmp/atprobe/dir/renamed");
    unlink("/tmp/atprobe/dir/nested");
    unlink("/tmp/atprobe/hard");
    unlink("/tmp/atprobe/sym");
    unlink("/tmp/atprobe/file");
    unlink("/tmp/atprobe/basefile");
    unlink("/tmp/atprobe/abs");
    unlink("/tmp/atprobe/abs2");
    rmdir("/tmp/atprobe/dir");
    rmdir("/tmp/atprobe");
}

int main(void) {
    struct stat st;
    struct stat lst;
    struct timespec ts[2];
    char buf[64];
    int dirfd;
    int subfd;
    int filefd;
    int fd;
    ssize_t n;

    puts("atprobe start");
    cleanup();

    check("mkdir root", mkdir("/tmp/atprobe", 0777) == 0);
    dirfd = open("/tmp/atprobe", O_RDONLY | O_DIRECTORY);
    check("open dirfd", dirfd >= 0);
    if (dirfd < 0) {
        printf("atprobe FAIL\n");
        return 1;
    }

    fd = openat(dirfd, "file", O_CREAT | O_RDWR, 0644);
    check("openat create", fd >= 0);
    if (fd >= 0) {
        check("openat write", write(fd, "abc", 3) == 3);
        close(fd);
    }
    check("fstatat file", fstatat(dirfd, "file", &st, 0) == 0 &&
                            S_ISREG(st.st_mode) && st.st_size == 3);

    filefd = openat(dirfd, "basefile", O_CREAT | O_RDWR, 0644);
    check("openat basefile", filefd >= 0);
    errno = 0;
    check("openat nondir base", openat(filefd, "child", O_CREAT | O_RDWR, 0644) < 0 &&
                                 errno == ENOTDIR);
    errno = 0;
    check("fstatat nondir base", fstatat(filefd, "child", &st, 0) < 0 &&
                                  errno == ENOTDIR);
    fd = openat(filefd, "/tmp/atprobe/abs", O_CREAT | O_RDWR, 0644);
    check("openat absolute override", fd >= 0);
    if (fd >= 0) close(fd);

    check("mkdirat dir", mkdirat(dirfd, "dir", 0755) == 0);
    subfd = openat(dirfd, "dir", O_RDONLY | O_DIRECTORY);
    check("openat subdir", subfd >= 0);
    if (subfd < 0) {
        close(dirfd);
        cleanup();
        printf("atprobe FAIL\n");
        return 1;
    }

    fd = openat(subfd, "nested", O_CREAT | O_WRONLY, 0644);
    check("openat nested", fd >= 0);
    if (fd >= 0) close(fd);
    check("renameat into subdir", renameat(dirfd, "file", subfd, "renamed") == 0);
    check("fstatat renamed", fstatat(subfd, "renamed", &st, 0) == 0 &&
                              S_ISREG(st.st_mode));
    check("linkat hard", linkat(subfd, "renamed", dirfd, "hard", 0) == 0);
    check("linkat nlink", fstatat(dirfd, "hard", &st, 0) == 0 && st.st_nlink >= 2);

    check("symlinkat create", symlinkat("dir/renamed", dirfd, "sym") == 0);
    n = readlinkat(dirfd, "sym", buf, sizeof(buf) - 1);
    if (n >= 0) buf[n] = '\0';
    check("readlinkat target", n == (ssize_t)strlen("dir/renamed") &&
                                strcmp(buf, "dir/renamed") == 0);
    check("fstatat nofollow", fstatat(dirfd, "sym", &lst, AT_SYMLINK_NOFOLLOW) == 0 &&
                              S_ISLNK(lst.st_mode));
    check("fstatat follows", fstatat(dirfd, "sym", &st, 0) == 0 &&
                             S_ISREG(st.st_mode));

    ts[0].tv_sec = 1234;
    ts[0].tv_nsec = 0;
    ts[1].tv_sec = 1235;
    ts[1].tv_nsec = 0;
    check("utimensat nofollow", utimensat(dirfd, "sym", ts, AT_SYMLINK_NOFOLLOW) == 0 &&
                                fstatat(dirfd, "sym", &lst, AT_SYMLINK_NOFOLLOW) == 0 &&
                                lst.st_mtime == 1235);
    ts[0].tv_sec = 2234;
    ts[1].tv_sec = 2235;
    check("utimensat follows", utimensat(dirfd, "sym", ts, 0) == 0 &&
                              fstatat(subfd, "renamed", &st, 0) == 0 &&
                              st.st_mtime == 2235);

    check("unlinkat hard", unlinkat(dirfd, "hard", 0) == 0);
    check("unlinkat sym", unlinkat(dirfd, "sym", 0) == 0);
    check("unlinkat renamed", unlinkat(subfd, "renamed", 0) == 0);
    check("unlinkat nested", unlinkat(subfd, "nested", 0) == 0);
    close(subfd);
    check("unlinkat removedir", unlinkat(dirfd, "dir", AT_REMOVEDIR) == 0);
    close(filefd);
    check("unlinkat abs", unlinkat(AT_FDCWD, "/tmp/atprobe/abs", 0) == 0);
    check("unlinkat basefile", unlinkat(dirfd, "basefile", 0) == 0);
    close(dirfd);
    check("cleanup root", rmdir("/tmp/atprobe") == 0);

    printf("atprobe %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
