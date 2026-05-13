#include "unistd.h"
#include "fcntl.h"
#include "limits.h"
#include "poll.h"
#include "sys/wait.h"
#include "stdio.h"
#include "errno.h"
#include "string.h"

#define BIG_SIZE (PIPE_BUF + 123)

static char big_in[BIG_SIZE];
static char big_out[BIG_SIZE];

static int write_all(int fd, const char* buf, int len) {
    int done = 0;
    while (done < len) {
        int n = write(fd, buf + done, len - done);
        if (n <= 0) return -1;
        done += n;
    }
    return done;
}

static int read_until_eof(int fd, char* buf, int max) {
    int done = 0;
    for (;;) {
        int n = read(fd, buf + done, max - done);
        if (n < 0) return -1;
        if (n == 0) return done;
        done += n;
        if (done > max) return -1;
    }
}

static int check_large_transfer(void) {
    int fds[2];
    int status = 0;
    int got;
    int i;
    pid_t pid;

    for (i = 0; i < BIG_SIZE; i++) {
        big_in[i] = (char)('a' + (i % 23));
        big_out[i] = 0;
    }

    if (pipe(fds) < 0) {
        puts("pipeprobe large pipe: FAIL");
        return 1;
    }

    pid = fork();
    if (pid < 0) {
        puts("pipeprobe large fork: FAIL");
        return 1;
    }
    if (pid == 0) {
        close(fds[0]);
        if (write_all(fds[1], big_in, BIG_SIZE) != BIG_SIZE) return 2;
        close(fds[1]);
        return 0;
    }

    close(fds[1]);
    got = read_until_eof(fds[0], big_out, BIG_SIZE);
    close(fds[0]);
    if (waitpid(pid, &status, 0) != pid || status != 0 || got != BIG_SIZE ||
        memcmp(big_in, big_out, BIG_SIZE) != 0) {
        puts("pipeprobe large transfer: FAIL");
        return 1;
    }
    return 0;
}

static int check_nonblock_full_atomic(void) {
    int fds[2];
    int total = 0;
    char chunk[128];
    char one = 'x';
    char two[2] = { 'y', 'z' };
    int i;

    for (i = 0; i < (int)sizeof(chunk); i++) chunk[i] = 'p';

    if (pipe2(fds, O_NONBLOCK) < 0) {
        puts("pipeprobe full pipe: FAIL");
        return 1;
    }
    for (;;) {
        int n = write(fds[1], chunk, sizeof(chunk));
        if (n < 0) {
            if (errno != EAGAIN) {
                puts("pipeprobe fill errno: FAIL");
                return 1;
            }
            break;
        }
        total += n;
    }
    if (total != PIPE_BUF) {
        puts("pipeprobe fill size: FAIL");
        return 1;
    }
    if (read(fds[0], &one, 1) != 1) {
        puts("pipeprobe drain byte: FAIL");
        return 1;
    }
    if (write(fds[1], two, 2) != -1 || errno != EAGAIN) {
        puts("pipeprobe atomic nonblock: FAIL");
        return 1;
    }
    if (write(fds[1], two, 1) != 1) {
        puts("pipeprobe atomic one: FAIL");
        return 1;
    }
    close(fds[0]);
    close(fds[1]);
    return 0;
}

static int check_poll(void) {
    int fds[2];
    struct pollfd pfd;
    char c = 'q';

    if (pipe(fds) < 0) {
        puts("pipeprobe poll pipe: FAIL");
        return 1;
    }
    pfd.fd = fds[0];
    pfd.events = POLLIN | POLLHUP;
    pfd.revents = 0;
    if (poll(&pfd, 1, 0) != 0) {
        puts("pipeprobe poll empty: FAIL");
        return 1;
    }
    if (write(fds[1], &c, 1) != 1) {
        puts("pipeprobe poll write: FAIL");
        return 1;
    }
    pfd.revents = 0;
    if (poll(&pfd, 1, 0) != 1 || (pfd.revents & POLLIN) == 0) {
        puts("pipeprobe poll readable: FAIL");
        return 1;
    }
    if (read(fds[0], &c, 1) != 1) {
        puts("pipeprobe poll read: FAIL");
        return 1;
    }
    close(fds[1]);
    pfd.revents = 0;
    if (poll(&pfd, 1, 0) != 1 ||
        (pfd.revents & (POLLIN | POLLHUP)) != (POLLIN | POLLHUP)) {
        puts("pipeprobe poll hup: FAIL");
        return 1;
    }
    close(fds[0]);
    return 0;
}

int main(void) {
    int fds[2];
    char buf[8];

    if (pipe(fds) < 0) {
        puts("pipeprobe pipe: FAIL");
        return 1;
    }
    if (write(fds[1], "ok", 2) != 2 || read(fds[0], buf, 2) != 2 ||
        buf[0] != 'o' || buf[1] != 'k') {
        puts("pipeprobe rw: FAIL");
        return 1;
    }
    close(fds[1]);
    if (read(fds[0], buf, 1) != 0) {
        puts("pipeprobe eof: FAIL");
        return 1;
    }
    close(fds[0]);

    if (pipe(fds) < 0) {
        puts("pipeprobe pipe2: FAIL");
        return 1;
    }
    close(fds[0]);
    if (write(fds[1], "x", 1) != -1 || errno != EPIPE) {
        puts("pipeprobe epipe: FAIL");
        return 1;
    }
    close(fds[1]);

    if (pipe2(fds, O_NONBLOCK | O_CLOEXEC) < 0) {
        puts("pipeprobe pipe2 flags: FAIL");
        return 1;
    }
    if (read(fds[0], buf, 1) != -1 || errno != EAGAIN) {
        puts("pipeprobe nonblock: FAIL");
        return 1;
    }
    close(fds[0]);
    close(fds[1]);
    if (check_nonblock_full_atomic() != 0) return 1;
    if (check_poll() != 0) return 1;
    if (check_large_transfer() != 0) return 1;
    puts("pipeprobe: PASS");
    return 0;
}
