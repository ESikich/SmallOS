#ifndef USER_SMALLOS_DOS_H
#define USER_SMALLOS_DOS_H

#include "dirent.h"

#define FA_RDONLY 0x01
#define FA_HIDDEN 0x02
#define FA_SYSTEM 0x04
#define FA_LABEL  0x08
#define FA_DIREC  0x10
#define FA_ARCH   0x20

#define SMALLOS_DOS_FIND_DIR_MAX 160u
#define SMALLOS_DOS_FIND_PATTERN_MAX 32u

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

struct ffblk {
    char ff_reserved[21];
    char ff_attrib;
    unsigned ff_ftime;
    unsigned ff_fdate;
    long ff_fsize;
    char ff_name[13];
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

typedef struct smallos_dos_find {
    DIR* dir;
    char dirname[SMALLOS_DOS_FIND_DIR_MAX];
    char pattern[SMALLOS_DOS_FIND_PATTERN_MAX];
} smallos_dos_find_t;

int _dos_getdate(struct dosdate_t* date);
int _dos_gettime(struct dostime_t* time);
int _dos_open(const char* path, unsigned mode, int* handle);
int _dos_creat(const char* path, unsigned attr, int* handle);
int _dos_close(int handle);
int _dos_read(int handle, void* buf, unsigned count, unsigned* got);
int _dos_write(int handle, const void* buf, unsigned count, unsigned* wrote);
int _dos_getdiskfree(unsigned drive, struct diskfree_t* info);

long smallos_filelength(int fd);
void movedata(unsigned srcseg, unsigned srcoff, unsigned dstseg,
              unsigned dstoff, unsigned len);
void* getvect(unsigned vector);
void setvect(unsigned vector, void* handler);
void disable(void);
void enable(void);
int int86(int intno, union REGS* inregs, union REGS* outregs);
int int86x(int intno, union REGS* inregs, union REGS* outregs,
           struct SREGS* segregs);
void smallos_dos_geninterrupt(unsigned int intno);
int inp(unsigned port);
int inportb(unsigned port);
unsigned inport(unsigned port);
int outp(unsigned port, int value);
void outportb(unsigned port, int value);
void outport(unsigned port, unsigned value);
int smallos_dos_findfirst(smallos_dos_find_t* find, const char* pattern,
                          struct ffblk* out, unsigned attrib);
int smallos_dos_findnext(smallos_dos_find_t* find, struct ffblk* out);
void smallos_dos_findclose(smallos_dos_find_t* find);

#endif /* USER_SMALLOS_DOS_H */
