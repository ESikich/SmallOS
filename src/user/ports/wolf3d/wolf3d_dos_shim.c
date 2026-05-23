#include "wolf3d_port.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "DIR.H"

static DIR* wolf3d_find_dir;
static char wolf3d_find_dirname[160];
static char wolf3d_find_pattern[32];

int wolf3d_argc;
char** wolf3d_argv;
unsigned int wolf3d_reg_ax;
unsigned int wolf3d_reg_bx;
unsigned int wolf3d_reg_cx;
unsigned int wolf3d_reg_dx;
unsigned int wolf3d_reg_di;
unsigned int wolf3d_reg_si;

static char* wolf3d_format_long(long value, unsigned long uvalue,
                                int is_signed, char* str, int base) {
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char tmp[34];
    unsigned long n;
    unsigned int pos = 0;
    unsigned int out = 0;

    if (!str || base < 2 || base > 36) {
        return str;
    }
    if (is_signed && value < 0 && base == 10) {
        str[out++] = '-';
        n = (unsigned long)(-value);
    } else {
        n = is_signed ? (unsigned long)value : uvalue;
    }
    do {
        tmp[pos++] = digits[n % (unsigned long)base];
        n /= (unsigned long)base;
    } while (n && pos < sizeof(tmp));
    while (pos) {
        str[out++] = tmp[--pos];
    }
    str[out] = '\0';
    return str;
}

char* ltoa(long value, char* str, int base) {
    return wolf3d_format_long(value, 0, 1, str, base);
}

char* ultoa(unsigned long value, char* str, int base) {
    return wolf3d_format_long(0, value, 0, str, base);
}

char* itoa(int value, char* str, int base) {
    return ltoa((long)value, str, base);
}

long wolf3d_filelength(int fd) {
    long cur = lseek(fd, 0, SEEK_CUR);
    long end;

    if (cur < 0) {
        return -1;
    }
    end = lseek(fd, 0, SEEK_END);
    (void)lseek(fd, cur, SEEK_SET);
    return end;
}

int _dos_getdate(struct dosdate_t* date) {
    time_t now;
    struct tm* tm;

    if (!date) {
        return -1;
    }
    now = time(NULL);
    tm = localtime(&now);
    if (!tm) {
        memset(date, 0, sizeof(*date));
        return -1;
    }
    date->day = (unsigned char)tm->tm_mday;
    date->month = (unsigned char)(tm->tm_mon + 1);
    date->year = (unsigned int)(tm->tm_year + 1900);
    date->dayofweek = (unsigned char)tm->tm_wday;
    return 0;
}

int _dos_gettime(struct dostime_t* time_out) {
    time_t now;
    struct tm* tm;

    if (!time_out) {
        return -1;
    }
    now = time(NULL);
    tm = localtime(&now);
    if (!tm) {
        memset(time_out, 0, sizeof(*time_out));
        return -1;
    }
    time_out->hour = (unsigned char)tm->tm_hour;
    time_out->minute = (unsigned char)tm->tm_min;
    time_out->second = (unsigned char)tm->tm_sec;
    time_out->hsecond = 0;
    return 0;
}

int _dos_open(const char* path, unsigned mode, int* handle) {
    int fd = open(path, (int)mode);

    if (fd < 0) {
        return errno;
    }
    if (handle) {
        *handle = fd;
    }
    return 0;
}

int _dos_creat(const char* path, unsigned attr, int* handle) {
    int fd;

    (void)attr;
    fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) {
        return errno;
    }
    if (handle) {
        *handle = fd;
    }
    return 0;
}

int creat(const char* path, unsigned mode) {
    return open(path, O_CREAT | O_TRUNC | O_WRONLY, (mode_t)mode);
}

int _dos_close(int handle) {
    return close(handle) < 0 ? errno : 0;
}

int _dos_read(int handle, void* buf, unsigned count, unsigned* got) {
    ssize_t rc = read(handle, buf, count);

    if (rc < 0) {
        return errno;
    }
    if (got) {
        *got = (unsigned)rc;
    }
    return 0;
}

int _dos_write(int handle, const void* buf, unsigned count, unsigned* wrote) {
    ssize_t rc = write(handle, buf, count);

    if (rc < 0) {
        return errno;
    }
    if (wrote) {
        *wrote = (unsigned)rc;
    }
    return 0;
}

int _dos_getdiskfree(unsigned drive, struct diskfree_t* info) {
    (void)drive;
    if (info) {
        info->total_clusters = 4096;
        info->avail_clusters = 4096;
        info->sectors_per_cluster = 8;
        info->bytes_per_sector = 512;
    }
    return 0;
}

void movedata(unsigned srcseg, unsigned srcoff, unsigned dstseg,
              unsigned dstoff, unsigned len) {
    (void)srcseg;
    (void)srcoff;
    (void)dstseg;
    (void)dstoff;
    (void)len;
}

void gotoxy(int x, int y) {
    (void)x;
    (void)y;
}

void clrscr(void) {
}

static int wolf3d_ascii_upper(int c) {
    return c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c;
}

static int wolf3d_dos_match(const char* pattern, const char* name) {
    while (*pattern) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) {
                return 1;
            }
            while (*name) {
                if (wolf3d_dos_match(pattern, name)) {
                    return 1;
                }
                name++;
            }
            return wolf3d_dos_match(pattern, name);
        }
        if (!*name) {
            return 0;
        }
        if (*pattern != '?' &&
            wolf3d_ascii_upper((unsigned char)*pattern) !=
                wolf3d_ascii_upper((unsigned char)*name)) {
            return 0;
        }
        pattern++;
        name++;
    }
    return *name == '\0';
}

static void wolf3d_split_find_pattern(const char* pattern, char* dir,
                                      size_t dir_size, char* file,
                                      size_t file_size) {
    const char* slash = pattern ? strrchr(pattern, '/') : NULL;

    if (slash) {
        size_t len = (size_t)(slash - pattern);
        if (len >= dir_size) {
            len = dir_size - 1u;
        }
        memcpy(dir, pattern, len);
        dir[len] = '\0';
        snprintf(file, file_size, "%s", slash + 1);
    } else {
        snprintf(dir, dir_size, ".");
        snprintf(file, file_size, "%s", pattern ? pattern : "");
    }
}

static int wolf3d_find_fill(struct ffblk* blk, const char* name) {
    char path[220];
    struct stat st;

    if (!blk || !name) {
        return -1;
    }
    snprintf(path, sizeof(path), "%s/%s", wolf3d_find_dirname, name);
    memset(blk, 0, sizeof(*blk));
    blk->ff_attrib = FA_ARCH;
    if (stat(path, &st) == 0) {
        blk->ff_fsize = (long)st.st_size;
    }
    snprintf(blk->ff_name, sizeof(blk->ff_name), "%s", name);
    return 0;
}

static int wolf3d_find_open(const char* dir) {
    if (wolf3d_find_dir) {
        closedir(wolf3d_find_dir);
        wolf3d_find_dir = NULL;
    }
    snprintf(wolf3d_find_dirname, sizeof(wolf3d_find_dirname), "%s", dir);
    wolf3d_find_dir = opendir(wolf3d_find_dirname);
    return wolf3d_find_dir ? 0 : -1;
}

int findfirst(const char* pattern, void* out, unsigned attrib) {
    (void)attrib;
    char dir[160];
    char file[32];

    wolf3d_split_find_pattern(pattern, dir, sizeof(dir), file, sizeof(file));
    snprintf(wolf3d_find_pattern, sizeof(wolf3d_find_pattern), "%s", file);
    if (wolf3d_find_open(dir) == 0 && findnext(out) == 0) {
        return 0;
    }
    if (!strchr(pattern ? pattern : "", '/') &&
        strcmp(dir, "/usr/share/wolf3d") != 0 &&
        wolf3d_find_open("/usr/share/wolf3d") == 0 &&
        findnext(out) == 0) {
        return 0;
    }
    if (wolf3d_find_dir) {
        closedir(wolf3d_find_dir);
        wolf3d_find_dir = NULL;
    }
    return -1;
}

int findnext(void* out) {
    struct dirent* ent;

    if (!wolf3d_find_dir) {
        return -1;
    }
    while ((ent = readdir(wolf3d_find_dir)) != NULL) {
        if (wolf3d_dos_match(wolf3d_find_pattern, ent->d_name)) {
            return wolf3d_find_fill((struct ffblk*)out, ent->d_name);
        }
    }
    closedir(wolf3d_find_dir);
    wolf3d_find_dir = NULL;
    return -1;
}

int getch(void) {
    return 0;
}

int kbhit(void) {
    return 0;
}

int bioskey(int command) {
    (void)command;
    return 0;
}
