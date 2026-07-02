#include "errno.h"
#include "fcntl.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "sys/mount.h"
#include "sys/stat.h"
#include "sys/vfs.h"

#define EXT2_SUPER_MAGIC 0xEF53L
#define PROC_SUPER_MAGIC 0x9FA0L
#define DEV_SUPER_MAGIC  0x01021994L

static int failures = 0;

static void check(const char* name, int ok) {
    printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static int read_contains(const char* path, const char* needle) {
    char buf[512];
    int fd;
    int n;

    fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    n = read(fd, buf, sizeof(buf) - 1u);
    close(fd);
    if (n < 0) return 0;
    buf[n] = '\0';
    return strstr(buf, needle) != 0;
}

int main(void) {
    struct statfs fs;
    int fd;

    puts("mountprobe start");

    check("mounts root line", read_contains("/proc/mounts", "rootfs / ext2 rw 0 0"));
    check("mounts proc line", read_contains("/proc/mounts", "proc /proc proc rw 0 0"));
    check("mounts dev line", read_contains("/proc/mounts", "dev /dev devtmpfs rw 0 0"));

    check("statfs root ext2", statfs("/", &fs) == 0 &&
                                fs.f_type == EXT2_SUPER_MAGIC &&
                                fs.f_bsize > 0 &&
                                fs.f_blocks >= fs.f_bfree);
    check("statfs proc", statfs("/proc", &fs) == 0 &&
                         fs.f_type == PROC_SUPER_MAGIC &&
                         fs.f_bsize > 0);
    check("statfs dev", statfs("/dev", &fs) == 0 &&
                        fs.f_type == DEV_SUPER_MAGIC &&
                        fs.f_bsize > 0);

    fd = open("/proc/mounts", O_RDONLY);
    check("open proc mounts", fd >= 0);
    if (fd >= 0) {
        check("fstatfs proc", fstatfs(fd, &fs) == 0 &&
                               fs.f_type == PROC_SUPER_MAGIC);
        close(fd);
    }

    fd = open("/dev/null", O_RDONLY);
    check("open dev null", fd >= 0);
    if (fd >= 0) {
        check("fstatfs dev", fstatfs(fd, &fs) == 0 &&
                              fs.f_type == DEV_SUPER_MAGIC);
        close(fd);
    }

    errno = 0;
    check("mount invalid type", mount("none", "/tmp", "notfs", 0, 0) < 0 &&
                                errno == EINVAL);
    errno = 0;
    check("mount root busy", mount("rootfs", "/", "ext2", 0, 0) < 0 &&
                             errno == EBUSY);
    errno = 0;
    check("mount dynamic gated", mount("rootfs", "/tmp", "ext2", 0, 0) < 0 &&
                                  errno == ENOSYS);
    errno = 0;
    check("umount root busy", umount("/") < 0 && errno == EBUSY);
    errno = 0;
    check("umount proc busy", umount2("/proc", MNT_DETACH) < 0 && errno == EBUSY);
    errno = 0;
    check("umount invalid target", umount("/tmp") < 0 && errno == EINVAL);

    printf("mountprobe %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
