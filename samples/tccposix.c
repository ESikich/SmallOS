#include <assert.h>
#include <dirent.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/dir.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

static int write_all(int fd, const char* s) {
    unsigned int len = (unsigned int)strlen(s);
    return write(fd, s, len) == (int)len;
}

static int io_probe(void) {
    const char* path = "var/tmp/tccposix.txt";
    const char* payload = "alpha\nbeta\n";
    char buf[32];
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) return 0;
    if (!write_all(fd, payload)) {
        close(fd);
        return 0;
    }
    if (lseek(fd, 0, SEEK_SET) != 0) {
        close(fd);
        return 0;
    }
    int n = read(fd, buf, sizeof(buf) - 1u);
    if (n < 0) {
        close(fd);
        return 0;
    }
    buf[n] = '\0';
    close(fd);
    return strcmp(buf, payload) == 0;
}

static int stat_probe(void) {
    struct stat st;
    int fd;
    int ok;

    if (stat("var/tmp/tccposix.txt", &st) < 0) return 0;
    if (!S_ISREG(st.st_mode) || st.st_size != 11) return 0;

    fd = open("var/tmp/tccposix.txt", O_RDONLY);
    if (fd < 0) return 0;
    ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 11;
    close(fd);
    return ok;
}

static int cwd_probe(void) {
    char cwd[128];
    char resolved[128];
    if (!getcwd(cwd, sizeof(cwd))) return 0;
    if (strcmp(cwd, "/") != 0) return 0;
    if (!realpath("var/tmp/../tmp/tccposix.txt", resolved)) return 0;
    return strcmp(resolved, "/var/tmp/tccposix.txt") == 0;
}

static int dir_probe(void) {
    DIR* dir = opendir("var/tmp/samples");
    int count = 0;
    int saw_self = 0;
    if (!dir) return 0;

    for (;;) {
        struct dirent* ent = readdir(dir);
        if (!ent) break;
        count++;
        if (strcmp(ent->d_name, "tccposix.c") == 0) {
            saw_self = 1;
        }
    }

    closedir(dir);
    return count >= 6 && saw_self;
}

static int time_probe(void) {
    struct timeval tv;
    return gettimeofday(&tv, 0) == 0 && tv.tv_sec >= 0 && tv.tv_usec >= 0;
}

static int header_probe(void) {
    char buf[8];
    struct direct ent;

    assert(BYTE_ORDER == LITTLE_ENDIAN);
    bzero(buf, sizeof(buf));
    bcopy("abc", buf, 4);
    ent.d_ino = 12;
    return RAND_MAX == 32767 && strcmp(buf, "abc") == 0 && bcmp(buf, "abc", 4) == 0 &&
           S_IREAD == S_IRUSR && O_RDONLY == 0 && ent.d_ino == 12;
}

static int system_probe(void) {
    int status = system("echo tcc system ok");
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int main(void) {
    int io = io_probe();
    int st = stat_probe();
    int cwd = cwd_probe();
    int dir = dir_probe();
    int tm = time_probe();
    int hdr = header_probe();
    int sys = system_probe();
    int total = io + st + cwd + dir + tm + hdr + sys;

    fprintf(stderr, "tcc posix stderr: %s\n", strerror(ENOENT));
    printf("tcc posix ok: io=%d stat=%d cwd=%d dir=%d time=%d headers=%d system=%d total=%d\n",
           io, st, cwd, dir, tm, hdr, sys, total);
    return total == 7 ? 0 : 1;
}
