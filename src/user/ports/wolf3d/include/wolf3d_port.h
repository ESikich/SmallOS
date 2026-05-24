#ifndef SMALLOS_WOLF3D_PORT_H
#define SMALLOS_WOLF3D_PORT_H

/*
 * Compatibility prelude for compiling the original Borland/DOS Wolf3D
 * sources as SmallOS user code. This file is intentionally limited to
 * language and platform shims; game behavior belongs in the upstream modules.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <smallos_dos.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef isprint
#define isprint(c) ((unsigned char)(c) >= 32u && (unsigned char)(c) < 127u)
#endif

#define far
#define near
#define huge
#define _seg
#define interrupt

#define cdecl
#define pascal

#define _argc wolf3d_argc
#define _argv wolf3d_argv

extern int wolf3d_argc;
extern char** wolf3d_argv;
extern unsigned int wolf3d_reg_ax;
extern unsigned int wolf3d_reg_bx;
extern unsigned int wolf3d_reg_cx;
extern unsigned int wolf3d_reg_dx;
extern unsigned int wolf3d_reg_di;
extern unsigned int wolf3d_reg_si;

#define _AX wolf3d_reg_ax
#define _BX wolf3d_reg_bx
#define _CX wolf3d_reg_cx
#define _DX wolf3d_reg_dx
#define _DI wolf3d_reg_di
#define _SI wolf3d_reg_si
#define _AH wolf3d_reg_ax
#define _AL wolf3d_reg_ax
#define _BH wolf3d_reg_bx
#define _BL wolf3d_reg_bx
#define _CH wolf3d_reg_cx
#define _CL wolf3d_reg_cx
#define _DH wolf3d_reg_dx
#define _DL wolf3d_reg_dx

#ifndef O_TEXT
#define O_TEXT 0
#endif

#ifndef O_BINARY
#define O_BINARY 0
#endif

#ifndef EINVFMT
#define EINVFMT EIO
#endif

typedef uint8_t wolf3d_u8;
typedef uint16_t wolf3d_u16;
typedef uint32_t wolf3d_u32;

void* wolf3d_screen_ptr(uint16_t ofs);
void wolf3d_vga_write_planar_byte(void* base, unsigned int byte_offset,
                                  unsigned int plane, uint8_t value);
uint8_t wolf3d_peekb(uint16_t seg, uint16_t ofs);
uint16_t wolf3d_peek(uint16_t seg, uint16_t ofs);
void wolf3d_pokeb(uint16_t seg, uint16_t ofs, uint8_t value);
void wolf3d_poke(uint16_t seg, uint16_t ofs, uint16_t value);
void wolf3d_input_poll(void);
void wolf3d_input_clear(void);
void wolf3d_reset_control_bindings(void);
void wolf3d_validate_control_config(void);
void wolf3d_read_config_file(int file, void* scores, unsigned int max_scores,
                             void* sd, void* sm, void* sds);
void wolf3d_write_config_file(int file, const void* scores,
                              unsigned int max_scores, int sd, int sm,
                              int sds);
void wolf3d_platform_shutdown(void);
__attribute__((noreturn)) void wolf3d_exit(int code);
void wolf3d_sync_time_count(void);
void wolf3d_geninterrupt(unsigned int intno);
void wolf3d_vga_clear_view(void);
void wolf3d_set_post_source(const void* source, unsigned int offset);
void wolf3d_scale_post(unsigned int x, unsigned int width,
                       unsigned int wall_height);
void wolf3d_set_line_scale(const void* scale);
void wolf3d_set_line_shape(const void* shape);
void wolf3d_scale_line(void);
void wolf3d_visual_smoke_frame(void);

#define exit(code) wolf3d_exit(code)

static inline void* wolf3d_mk_fp(uint16_t seg, uint16_t ofs) {
    if (seg == 0xa000u) {
        return wolf3d_screen_ptr(ofs);
    }
    return (void*)(uintptr_t)(((uint32_t)seg << 4) + ofs);
}

static inline uint16_t wolf3d_fp_seg(const void* p) {
    return (uint16_t)(((uintptr_t)p) >> 4);
}

static inline uint16_t wolf3d_fp_off(const void* p) {
    return (uint16_t)(((uintptr_t)p) & 0x0fu);
}

#define MK_FP(seg, ofs) wolf3d_mk_fp((uint16_t)(seg), (uint16_t)(ofs))
#define FP_SEG(ptr) wolf3d_fp_seg((const void*)(ptr))
#define FP_OFF(ptr) wolf3d_fp_off((const void*)(ptr))

#define _fmemcpy memcpy
#define _fmemset memset
#define _fmemcmp memcmp
#define _fmemmove memmove
#define _fstrcpy strcpy
#define _fstrlen strlen
#define _fstricmp stricmp

#define peekb(seg, ofs) wolf3d_peekb((uint16_t)(seg), (uint16_t)(ofs))
#define peek(seg, ofs) wolf3d_peek((uint16_t)(seg), (uint16_t)(ofs))
#define poke(seg, ofs, val) wolf3d_poke((uint16_t)(seg), (uint16_t)(ofs), (uint16_t)(val))
#define pokeb(seg, ofs, val) wolf3d_pokeb((uint16_t)(seg), (uint16_t)(ofs), (uint8_t)(val))
#define geninterrupt(intno) wolf3d_geninterrupt((unsigned int)(intno))
#define harderr(handler) ((void)0)
#define errout(message) ((void)0)

#define farmalloc(size) malloc(size)
#define farfree(ptr) free(ptr)

/*
 * Wolf3D's startup screen and memory checks are DOS-conventional-memory
 * concepts, not host RAM totals. Keep these values shaped like a healthy
 * Borland-era process while allocations themselves use the SmallOS heap.
 */
#define WOLF3D_DOS_NEAR_HEAP_BYTES (64L * 1024L)
#define WOLF3D_DOS_FAR_HEAP_BYTES (256L * 1024L)
#define WOLF3D_DOS_MAINMEM_BYTES \
    (WOLF3D_DOS_NEAR_HEAP_BYTES + WOLF3D_DOS_FAR_HEAP_BYTES)
#define coreleft() (WOLF3D_DOS_NEAR_HEAP_BYTES)
#define farcoreleft() (WOLF3D_DOS_FAR_HEAP_BYTES)

#ifdef UPLOAD
/*
 * The staged shareware data keeps wall and sprite art in VSWAP, while the
 * released source header numbers some text-art chunks after inline tile art.
 * The source-prep step remaps the graphics header to the data shape; these
 * names cover external text/demo chunks that are not part of graphicnums.
 */
#define T_HELPART 150
#define T_DEMO0 151
#define T_DEMO1 152
#define T_DEMO2 153
#define T_DEMO3 154
#define T_ENDART1 155
#define T_ENDART2 0
#define T_ENDART3 0
#define T_ENDART4 0
#define T_ENDART5 0
#define T_ENDART6 0
#define MAP4L10PATH_MAP 0
#define WOLF4_BOSS_MAP 0
#define WOLF4_MAP_1_MAP 0
#define WOLF4_MAP_2_MAP 0
#define WOLF4_MAP_3_MAP 0
#define WOLF4_MAP_4_MAP 0
#define WOLF4_MAP_5_MAP 0
#define WOLF4_MAP_6_MAP 0
#define WOLF4_MAP_7_MAP 0
#define WOLF4_MAP_8_MAP 0
#define WOLF4_SECRET_MAP 0
#define WOLF5_BOSS_MAP 0
#define WOLF5_MAP_1_MAP 0
#define WOLF5_MAP_2_MAP 0
#define WOLF5_MAP_3_MAP 0
#define WOLF5_MAP_4_MAP 0
#define WOLF5_MAP_5_MAP 0
#define WOLF5_MAP_6_MAP 0
#define WOLF5_MAP_7_MAP 0
#define WOLF5_MAP_8_MAP 0
#define WOLF5_SECRET_MAP 0
#define WOLF6_BOSS_MAP 0
#define WOLF6_MAP_1_MAP 0
#define WOLF6_MAP_2_MAP 0
#define WOLF6_MAP_3_MAP 0
#define WOLF6_MAP_4_MAP 0
#define WOLF6_MAP_5_MAP 0
#define WOLF6_MAP_6_MAP 0
#define WOLF6_MAP_7_MAP 0
#define WOLF6_MAP_8_MAP 0
#define WOLF6_SECRET_MAP 0
#endif

char* ltoa(long value, char* str, int base);
char* ultoa(unsigned long value, char* str, int base);
char* itoa(int value, char* str, int base);
void gotoxy(int x, int y);
void clrscr(void);

#endif /* SMALLOS_WOLF3D_PORT_H */
