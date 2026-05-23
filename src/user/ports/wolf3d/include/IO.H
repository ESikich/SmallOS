#ifndef SMALLOS_WOLF3D_IO_H
#define SMALLOS_WOLF3D_IO_H
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#define tell(fd) lseek((fd), 0, SEEK_CUR)
#define filelength(fd) wolf3d_filelength(fd)
long wolf3d_filelength(int fd);
#endif
