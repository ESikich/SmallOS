#include "errno.h"
#include "poll.h"
#include "sys/time.h"
#include "unistd.h"

#define SELECT_MAX_FDS 32

static int select_fd_isset(fd_set set, int fd) {
    return fd >= 0 && fd < SELECT_MAX_FDS && (set & (1u << fd)) != 0u;
}

static void select_fd_set(fd_set* set, int fd) {
    if (set && fd >= 0 && fd < SELECT_MAX_FDS) {
        *set |= (1u << fd);
    }
}

static int select_timeout_ms(struct timeval* timeout) {
    long ms;
    if (!timeout) {
        return -1;
    }
    if (timeout->tv_sec < 0 || timeout->tv_usec < 0) {
        errno = EINVAL;
        return -2;
    }
    ms = timeout->tv_sec * 1000L + (timeout->tv_usec + 999L) / 1000L;
    if (ms > 0x7fffffffL) {
        ms = 0x7fffffffL;
    }
    return (int)ms;
}

static int select_append_fd(struct pollfd* fds, int* count, int fd, short events) {
    for (int i = 0; i < *count; i++) {
        if (fds[i].fd == fd) {
            fds[i].events |= events;
            return 0;
        }
    }
    if (*count >= SELECT_MAX_FDS) {
        errno = EINVAL;
        return -1;
    }
    fds[*count].fd = fd;
    fds[*count].events = events;
    fds[*count].revents = 0;
    (*count)++;
    return 0;
}

int select(int nfds, fd_set* readfds, fd_set* writefds,
           fd_set* exceptfds, struct timeval* timeout) {
    struct pollfd fds[SELECT_MAX_FDS];
    int count = 0;
    int timeout_ms;
    fd_set in_read = readfds ? *readfds : 0u;
    fd_set in_write = writefds ? *writefds : 0u;
    fd_set in_except = exceptfds ? *exceptfds : 0u;
    fd_set out_read = 0u;
    fd_set out_write = 0u;
    fd_set out_except = 0u;
    int ready;
    int ready_fds = 0;

    if (nfds < 0 || nfds > SELECT_MAX_FDS) {
        errno = EINVAL;
        return -1;
    }

    timeout_ms = select_timeout_ms(timeout);
    if (timeout_ms == -2) {
        return -1;
    }

    for (int fd = 0; fd < nfds; fd++) {
        short events = 0;
        if (select_fd_isset(in_read, fd)) events |= POLLIN;
        if (select_fd_isset(in_write, fd)) events |= POLLOUT;
        if (select_fd_isset(in_except, fd)) events |= POLLPRI;
        if (events && select_append_fd(fds, &count, fd, events) < 0) {
            return -1;
        }
    }

    if (count == 0) {
        if (timeout_ms < 0) {
            for (;;) {
                usleep(0xffffffffu);
            }
        }
        if (timeout_ms > 0) {
            unsigned int usec = (timeout_ms > (int)(0xffffffffu / 1000u))
                              ? 0xffffffffu
                              : (unsigned int)timeout_ms * 1000u;
            usleep(usec);
        }
        if (readfds) *readfds = 0u;
        if (writefds) *writefds = 0u;
        if (exceptfds) *exceptfds = 0u;
        return 0;
    }

    ready = poll(fds, (nfds_t)count, timeout_ms);
    if (ready <= 0) {
        if (ready == 0) {
            if (readfds) *readfds = 0u;
            if (writefds) *writefds = 0u;
            if (exceptfds) *exceptfds = 0u;
        }
        return ready;
    }

    for (int i = 0; i < count; i++) {
        int fd = fds[i].fd;
        int fd_ready = 0;
        if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
            select_fd_set(&out_read, fd);
            fd_ready = 1;
        }
        if (fds[i].revents & (POLLOUT | POLLERR)) {
            select_fd_set(&out_write, fd);
            fd_ready = 1;
        }
        if (fds[i].revents & (POLLPRI | POLLERR)) {
            select_fd_set(&out_except, fd);
            fd_ready = 1;
        }
        if (fd_ready) {
            ready_fds++;
        }
    }

    if (readfds) *readfds = out_read;
    if (writefds) *writefds = out_write;
    if (exceptfds) *exceptfds = out_except;
    return ready_fds;
}
