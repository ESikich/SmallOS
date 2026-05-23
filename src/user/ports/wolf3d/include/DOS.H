#ifndef SMALLOS_WOLF3D_DOS_H
#define SMALLOS_WOLF3D_DOS_H
#include <stdint.h>
#include <stddef.h>

struct dostime_t {
    unsigned char hour, minute, second, hsecond;
};

struct dosdate_t {
    unsigned char day, month;
    unsigned int year;
    unsigned char dayofweek;
};

struct find_t {
    unsigned attrib;
    unsigned wr_time;
    unsigned wr_date;
    long size;
    char name[13];
};

struct diskfree_t {
    unsigned total_clusters;
    unsigned avail_clusters;
    unsigned sectors_per_cluster;
    unsigned bytes_per_sector;
};

union REGS {
    struct {
        unsigned int ax, bx, cx, dx, si, di, cflag, flags;
    } x;
    struct {
        unsigned char al, ah, bl, bh, cl, ch, dl, dh;
    } h;
};

struct SREGS {
    unsigned int es, cs, ss, ds;
};

#define FA_ARCH 0x20
#define getvect(vector) (0)
#define setvect(vector, handler) ((void)0)
#define disable() ((void)0)
#define enable() ((void)0)

int _dos_getdate(struct dosdate_t* date);
int _dos_gettime(struct dostime_t* time);
int _dos_open(const char* path, unsigned mode, int* handle);
int _dos_creat(const char* path, unsigned attr, int* handle);
int _dos_close(int handle);
int _dos_read(int handle, void* buf, unsigned count, unsigned* got);
int _dos_write(int handle, const void* buf, unsigned count, unsigned* wrote);
int _dos_getdiskfree(unsigned drive, struct diskfree_t* info);

#endif
