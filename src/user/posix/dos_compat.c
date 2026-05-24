#include "smallos_dos.h"

#include "errno.h"
#include "fcntl.h"
#include "smallos_fs.h"
#include "stdio.h"
#include "string.h"
#include "sys/stat.h"
#include "time.h"
#include "unistd.h"

static int dos_ascii_upper(int c) {
    return c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c;
}

static int dos_match(const char* pattern, const char* name) {
    while (*pattern) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) {
                return 1;
            }
            while (*name) {
                if (dos_match(pattern, name)) {
                    return 1;
                }
                name++;
            }
            return dos_match(pattern, name);
        }
        if (!*name) {
            return 0;
        }
        if (*pattern != '?' &&
            dos_ascii_upper((unsigned char)*pattern) !=
                dos_ascii_upper((unsigned char)*name)) {
            return 0;
        }
        pattern++;
        name++;
    }
    return *name == '\0';
}

static void dos_split_find_pattern(const char* pattern, char* dir,
                                   unsigned int dir_size, char* file,
                                   unsigned int file_size) {
    const char* slash = pattern ? strrchr(pattern, '/') : 0;

    if (slash) {
        unsigned int len = (unsigned int)(slash - pattern);
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

static void dos_join_path(char* out, unsigned int out_size,
                          const char* dir, const char* name) {
    if (dir && dir[0] == '/' && dir[1] == '\0') {
        snprintf(out, out_size, "/%s", name);
        return;
    }
    snprintf(out, out_size, "%s/%s", dir ? dir : ".", name ? name : "");
}

static int dos_find_fill(const smallos_dos_find_t* find,
                         struct ffblk* blk, const char* name) {
    char path[220];
    struct stat st;

    if (!find || !blk || !name) {
        return -1;
    }
    dos_join_path(path, sizeof(path), find->dirname, name);
    memset(blk, 0, sizeof(*blk));
    blk->ff_attrib = FA_ARCH;
    if (stat(path, &st) == 0) {
        blk->ff_fsize = (long)st.st_size;
        if (S_ISDIR(st.st_mode)) {
            blk->ff_attrib = FA_DIREC;
        }
    }
    snprintf(blk->ff_name, sizeof(blk->ff_name), "%s", name);
    return 0;
}

static int dos_errno(void) {
    return errno ? errno : EIO;
}

int _dos_getdate(struct dosdate_t* date) {
    time_t now;
    struct tm* tm;

    if (!date) {
        return EFAULT;
    }
    now = time(0);
    tm = localtime(&now);
    if (!tm) {
        memset(date, 0, sizeof(*date));
        return EIO;
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
        return EFAULT;
    }
    now = time(0);
    tm = localtime(&now);
    if (!tm) {
        memset(time_out, 0, sizeof(*time_out));
        return EIO;
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
        return dos_errno();
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
        return dos_errno();
    }
    if (handle) {
        *handle = fd;
    }
    return 0;
}

int _dos_close(int handle) {
    return close(handle) < 0 ? dos_errno() : 0;
}

int _dos_read(int handle, void* buf, unsigned count, unsigned* got) {
    ssize_t rc = read(handle, buf, count);

    if (rc < 0) {
        return dos_errno();
    }
    if (got) {
        *got = (unsigned)rc;
    }
    return 0;
}

int _dos_write(int handle, const void* buf, unsigned count, unsigned* wrote) {
    ssize_t rc = write(handle, buf, count);

    if (rc < 0) {
        return dos_errno();
    }
    if (wrote) {
        *wrote = (unsigned)rc;
    }
    return 0;
}

int _dos_getdiskfree(unsigned drive, struct diskfree_t* info) {
    sys_fsinfo_t fs;
    unsigned int bytes_per_sector = 512u;
    int rc;

    (void)drive;
    if (!info) {
        return EFAULT;
    }

    rc = smallos_fsinfo(&fs);
    if (rc < 0) {
        return -rc;
    }

    if (fs.cluster_bytes == 0u) {
        return EIO;
    }
    if ((fs.cluster_bytes % bytes_per_sector) != 0u) {
        bytes_per_sector = fs.cluster_bytes;
    }

    info->total_clusters = fs.total_clusters;
    info->avail_clusters = fs.free_clusters;
    info->sectors_per_cluster = fs.cluster_bytes / bytes_per_sector;
    info->bytes_per_sector = bytes_per_sector;
    if (info->sectors_per_cluster == 0u) {
        info->sectors_per_cluster = 1u;
    }
    return 0;
}

long smallos_filelength(int fd) {
    long cur = lseek(fd, 0, SEEK_CUR);
    long end;

    if (cur < 0) {
        return -1;
    }
    end = lseek(fd, 0, SEEK_END);
    (void)lseek(fd, cur, SEEK_SET);
    return end;
}

void movedata(unsigned srcseg, unsigned srcoff, unsigned dstseg,
              unsigned dstoff, unsigned len) {
    (void)srcseg;
    (void)srcoff;
    (void)dstseg;
    (void)dstoff;
    (void)len;
}

void* getvect(unsigned vector) {
    (void)vector;
    return 0;
}

void setvect(unsigned vector, void* handler) {
    (void)vector;
    (void)handler;
}

void disable(void) {
}

void enable(void) {
}

int int86(int intno, union REGS* inregs, union REGS* outregs) {
    (void)intno;
    if (outregs && inregs) {
        *outregs = *inregs;
    }
    return 0;
}

int int86x(int intno, union REGS* inregs, union REGS* outregs,
           struct SREGS* segregs) {
    (void)segregs;
    return int86(intno, inregs, outregs);
}

void smallos_dos_geninterrupt(unsigned int intno) {
    (void)intno;
}

int inp(unsigned port) {
    (void)port;
    return 0;
}

int inportb(unsigned port) {
    (void)port;
    return 0;
}

unsigned inport(unsigned port) {
    (void)port;
    return 0u;
}

int outp(unsigned port, int value) {
    (void)port;
    return value;
}

void outportb(unsigned port, int value) {
    (void)port;
    (void)value;
}

void outport(unsigned port, unsigned value) {
    (void)port;
    (void)value;
}

int smallos_dos_findfirst(smallos_dos_find_t* find, const char* pattern,
                          struct ffblk* out, unsigned attrib) {
    char dir[SMALLOS_DOS_FIND_DIR_MAX];
    char file[SMALLOS_DOS_FIND_PATTERN_MAX];

    (void)attrib;
    if (!find || !out) {
        return -1;
    }

    smallos_dos_findclose(find);
    dos_split_find_pattern(pattern, dir, sizeof(dir), file, sizeof(file));
    snprintf(find->dirname, sizeof(find->dirname), "%s", dir);
    snprintf(find->pattern, sizeof(find->pattern), "%s", file);
    find->dir = opendir(find->dirname);
    if (find->dir && smallos_dos_findnext(find, out) == 0) {
        return 0;
    }
    smallos_dos_findclose(find);
    return -1;
}

int smallos_dos_findnext(smallos_dos_find_t* find, struct ffblk* out) {
    struct dirent* ent;

    if (!find || !find->dir || !out) {
        return -1;
    }
    while ((ent = readdir(find->dir)) != 0) {
        if (dos_match(find->pattern, ent->d_name)) {
            return dos_find_fill(find, out, ent->d_name);
        }
    }
    smallos_dos_findclose(find);
    return -1;
}

void smallos_dos_findclose(smallos_dos_find_t* find) {
    if (!find) {
        return;
    }
    if (find->dir) {
        closedir(find->dir);
    }
    memset(find, 0, sizeof(*find));
}

static smallos_dos_find_t s_dos_find;

__attribute__((weak)) int findfirst(const char* pattern, void* out,
                                    unsigned attrib) {
    return smallos_dos_findfirst(&s_dos_find, pattern, (struct ffblk*)out,
                                 attrib);
}

__attribute__((weak)) int findnext(void* out) {
    return smallos_dos_findnext(&s_dos_find, (struct ffblk*)out);
}
