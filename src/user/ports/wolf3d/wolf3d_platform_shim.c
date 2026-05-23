#include "wolf3d_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>

#include "WL_DEF.H"
#include "keyboard.h"
#include "uapi_input.h"
#include "uapi_sound.h"
#include "uapi_syscall.h"
#include "wolf3d_palette.h"
#include "wolf3d_signon.h"

#define WOLF3D_DISPLAY_INFO 53
#define WOLF3D_DISPLAY_FILL 54
#define WOLF3D_DISPLAY_BLIT 55
#define WOLF3D_DISPLAY_ACQUIRE 56
#define WOLF3D_DISPLAY_RELEASE 57
#define WOLF3D_DISPLAY_MAP 95
#define WOLF3D_DISPLAY_PRESENT_PAGE 96
#define WOLF3D_INPUT_READ 59
#define WOLF3D_SYS_EXIT 2
#define WOLF3D_GET_TICKS 3
#define WOLF3D_DISPLAY_FORMAT_XRGB8888 1u
#define WOLF3D_SCREEN_W 320u
#define WOLF3D_SCREEN_H 200u
#define WOLF3D_SCREEN_PIXELS (WOLF3D_SCREEN_W * WOLF3D_SCREEN_H)
#define WOLF3D_VIDEO_PAGES 3u
#define WOLF3D_PAGE_BYTES (80u * 208u)
#define WOLF3D_PAGE1START 0u
#define WOLF3D_PAGE2START WOLF3D_PAGE_BYTES
#define WOLF3D_PAGE3START (WOLF3D_PAGE_BYTES * 2u)
#define WOLF3D_VISIBLE_STRIDE 80u
#define WOLF3D_VGA_BYTES 65536u
#define WOLF3D_TICS_PER_SECOND 70u
#define WOLF3D_SMALLOS_TICS_PER_SECOND 300u
#define WOLF3D_TIMER_UNITS_PER_SECOND 2100u
#define WOLF3D_TIMER_UNITS_PER_TIC \
    (WOLF3D_TIMER_UNITS_PER_SECOND / WOLF3D_TICS_PER_SECOND)
#define WOLF3D_TIMER_UNITS_PER_SMALLOS_TIC \
    (WOLF3D_TIMER_UNITS_PER_SECOND / WOLF3D_SMALLOS_TICS_PER_SECOND)
#define WOLF3D_DEG90 (FINEANGLES / 4)
#define WOLF3D_DEG180 (FINEANGLES / 2)
#define WOLF3D_DEG270 (FINEANGLES * 3 / 4)
#define WOLF3D_DEG360 FINEANGLES
#define WOLF3D_MOUSE_INT 0x33u
#define WOLF3D_MOUSE_RESET 0u
#define WOLF3D_MOUSE_BUTTONS 3u
#define WOLF3D_MOUSE_SET_POSITION 4u
#define WOLF3D_MOUSE_DELTA 11u
#define WOLF3D_MOUSE_MAX_DELTA 32767
#define WOLF3D_MOUSE_MIN_DELTA (-32768)
#define WOLF3D_MOUSE_CENTER 120
#ifndef WOLF3D_ENABLE_SB_DIGI
#define WOLF3D_ENABLE_SB_DIGI 1
#endif
#define WOLF3D_DIGI_SAMPLE_HZ 7000u
#define WOLF3D_DIGI_BYTES_PER_TIC \
    ((WOLF3D_DIGI_SAMPLE_HZ + WOLF3D_TICS_PER_SECOND - 1u) / WOLF3D_TICS_PER_SECOND)
#define WOLF3D_ADLIB_MUSIC_HZ 700u
#define WOLF3D_ADLIB_SOUND_HZ 140u
#define WOLF3D_ADLIB_MUSIC_UNIT \
    (WOLF3D_TIMER_UNITS_PER_SECOND / WOLF3D_ADLIB_MUSIC_HZ)
#define WOLF3D_ADLIB_SOUND_UNIT \
    (WOLF3D_TIMER_UNITS_PER_SECOND / WOLF3D_ADLIB_SOUND_HZ)
#define WOLF3D_ADLIB_MAX_EVENTS_PER_POLL 512u
#define WOLF3D_AL_CHAR 0x20u
#define WOLF3D_AL_SCALE 0x40u
#define WOLF3D_AL_ATTACK 0x60u
#define WOLF3D_AL_SUS 0x80u
#define WOLF3D_AL_WAVE 0xe0u
#define WOLF3D_AL_FREQ_L 0xa0u
#define WOLF3D_AL_FREQ_H 0xb0u
#define WOLF3D_AL_FEED_CON 0xc0u
#define WOLF3D_AL_EFFECTS 0xbdu
#define WOLF3D_SOUND_COMMON_LENGTH 0u
#define WOLF3D_SOUND_COMMON_PRIORITY 4u
#define WOLF3D_PC_SOUND_DATA 6u
#define WOLF3D_AL_SOUND_INST 6u
#define WOLF3D_AL_SOUND_BLOCK 22u
#define WOLF3D_AL_SOUND_DATA 23u

typedef struct wolf3d_display_info {
    unsigned int width;
    unsigned int height;
    unsigned int pitch;
    unsigned int bpp;
    unsigned int format;
} wolf3d_display_info_t;

typedef struct wolf3d_display_fill_rect {
    unsigned int x;
    unsigned int y;
    unsigned int w;
    unsigned int h;
    unsigned int color;
} wolf3d_display_fill_rect_t;

typedef struct wolf3d_display_blit_rect {
    unsigned int x;
    unsigned int y;
    unsigned int w;
    unsigned int h;
    const unsigned int* pixels;
} wolf3d_display_blit_rect_t;

typedef struct wolf3d_display_map_info {
    unsigned int base;
    unsigned int width;
    unsigned int height;
    unsigned int pitch;
    unsigned int bpp;
    unsigned int format;
    unsigned int page_bytes;
    unsigned int page_count;
    unsigned int draw_page;
} wolf3d_display_map_info_t;

typedef struct wolf3d_display_present_page {
    unsigned int page;
    unsigned int next_page;
} wolf3d_display_present_page_t;

mminfotype mminfo;
memptr bufferseg;
boolean mmerror;
void (*beforesort)(void);
void (*aftersort)(void);

unsigned bufferofs;
unsigned displayofs;
unsigned pelpan;
unsigned screenseg = SCREENSEG;
unsigned linewidth = SCREENWIDTH;
unsigned ylookup[MAXSCANLINES];
boolean screenfaded;
unsigned bordercolor;

pictabletype _seg* pictable;
pictabletype _seg* picmtable;
spritetabletype _seg* spritetable;
byte fontcolor;
byte backcolor;
int fontnumber;
int px;
int py;
int bufferwidth;
int bufferheight;

word NumDigi;
word _seg* DigiList;

boolean AdLibPresent;
boolean SoundSourcePresent;
boolean SoundBlasterPresent;
boolean NeedsMusic;
boolean SoundPositioned;
SDMode SoundMode;
SDSMode DigiMode;
SMMode MusicMode;
boolean DigiPlaying;
int DigiMap[LASTSOUND];
longword TimeCount;

static byte wolf3d_palette[768];
static unsigned wolf3d_rnd_state = 1;
static byte wolf3d_video_planes[4][WOLF3D_VGA_BYTES];
static byte wolf3d_video_pages[WOLF3D_VIDEO_PAGES][WOLF3D_SCREEN_PIXELS];
static unsigned int wolf3d_xrgb[WOLF3D_SCREEN_PIXELS];
static unsigned int* wolf3d_scaled_xrgb;
static unsigned int wolf3d_scaled_capacity;
static uint32_t wolf3d_timer_base_units;
static longword wolf3d_timer_observed;
static int wolf3d_timer_ready;
static wolf3d_display_info_t wolf3d_display;
static int wolf3d_display_acquired;
static int wolf3d_display_checked;
static int wolf3d_display_cleared;
static int wolf3d_video_dirty;
static int wolf3d_video_deferred_present;
static int wolf3d_display_mapped;
static wolf3d_display_map_info_t wolf3d_display_map;
static int wolf3d_mouse_dx;
static int wolf3d_mouse_dy;
static int wolf3d_mouse_x = WOLF3D_MOUSE_CENTER;
static int wolf3d_mouse_y = WOLF3D_MOUSE_CENTER;
static unsigned int wolf3d_mouse_buttons;
static const byte* wolf3d_post_source;
static const t_compscale* wolf3d_line_scale;
static const byte* wolf3d_line_shape;
static const byte* wolf3d_pc_sound_data;
static longword wolf3d_pc_sound_length;
static longword wolf3d_pc_sound_end_tick;
static word wolf3d_pc_sound_number;
static word wolf3d_pc_sound_priority;
static byte* wolf3d_digi_buffer;
static unsigned int wolf3d_digi_buffer_capacity;
static longword wolf3d_digi_end_tick;
static uint32_t wolf3d_digi_end_units;
static word wolf3d_digi_number;
static word wolf3d_digi_priority;
static const byte wolf3d_al_carriers[9] = { 3, 4, 5, 11, 12, 13, 19, 20, 21 };
static const byte wolf3d_al_modifiers[9] = { 0, 1, 2, 8, 9, 10, 16, 17, 18 };
static const Instrument wolf3d_al_zero_inst = { 0 };
static byte* wolf3d_al_sound_data;
static longword wolf3d_al_sound_length;
static uint32_t wolf3d_al_sound_next_units;
static word wolf3d_al_sound_number;
static word wolf3d_al_sound_priority;
static byte wolf3d_al_block;
static word* wolf3d_music_data;
static word* wolf3d_music_ptr;
static unsigned int wolf3d_music_len_bytes;
static unsigned int wolf3d_music_left_bytes;
static uint32_t wolf3d_music_next_units;
static int wolf3d_music_active;

void HitHorizWall(void);
void HitVertWall(void);
void HitHorizDoor(void);
void HitVertDoor(void);
void HitHorizPWall(void);
void HitVertPWall(void);

extern int slinex;
extern int slinewidth;
extern uint16_t* linecmds;
int wolf3d_probe_stop_after_demo_prelude;
int wolf3d_probe_stop_after_title_frame;
int wolf3d_probe_stop_after_control_panel_frame;
int wolf3d_probe_stop_after_first_game_frame;

extern unsigned vgaCeiling[];
extern int midangle;
extern int focaltx;
extern int focalty;
extern unsigned xpartialup;
extern unsigned xpartialdown;
extern unsigned ypartialup;
extern unsigned ypartialdown;
extern unsigned tilehit;
extern unsigned pixx;
extern int xtile;
extern int ytile;
extern int xtilestep;
extern int ytilestep;
extern long xintercept;
extern long yintercept;
extern long xstep;
extern long ystep;
int CalcHeight(void);

static int wolf3d_syscall0(int num) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num) : "memory");
    return ret;
}

static int wolf3d_syscall1(int num, unsigned int arg1) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num), "b"(arg1) : "memory");
    return ret;
}

static int wolf3d_syscall3(int num, unsigned int arg1, unsigned int arg2,
                           unsigned int arg3) {
    int ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(num), "b"(arg1), "c"(arg2), "d"(arg3)
                     : "memory");
    return ret;
}

static int wolf3d_sound_op(unsigned int op, unsigned int arg1,
                           unsigned int arg2) {
    return wolf3d_syscall3(SYS_SOUND_OP, op, arg1, arg2);
}

static void wolf3d_planar_store(unsigned int offset, unsigned int plane,
                                byte color);
static byte wolf3d_planar_load(unsigned int offset, unsigned int plane);
static byte wolf3d_palette_channel(int value);
static void wolf3d_wait_tics(unsigned int ticks);
static void wolf3d_fade_wait_vbl(void);
static void wolf3d_display_clear(void);
static void wolf3d_display_release(void);
static unsigned int wolf3d_display_scale(void);
static int wolf3d_display_scale_buffer(unsigned int page, unsigned int scale,
                                       unsigned int* out_w,
                                       unsigned int* out_h,
                                       unsigned int** out_pixels);
static void wolf3d_video_flush_deferred_present(void);
static void wolf3d_video_copy_bytes(unsigned int source, unsigned int dest,
                                    unsigned int width_bytes,
                                    unsigned int height);
static void wolf3d_video_present(void);
static void wolf3d_video_commit_buffer(void);
static void wolf3d_sound_service(uint32_t now_units);
static void wolf3d_sound_stop(void);
static int wolf3d_play_digi_sample(word page, unsigned int length);
static void wolf3d_adlib_music_clear(void);

void* wolf3d_screen_ptr(uint16_t ofs) {
    return &wolf3d_video_planes[0][ofs];
}

uint8_t wolf3d_peekb(uint16_t seg, uint16_t ofs) {
    if (seg == SCREENSEG) {
        return wolf3d_planar_load(ofs, 0);
    }
    return 0;
}

uint16_t wolf3d_peek(uint16_t seg, uint16_t ofs) {
    uint16_t low = wolf3d_peekb(seg, ofs);
    uint16_t high = wolf3d_peekb(seg, (uint16_t)(ofs + 1u));

    return (uint16_t)(low | (high << 8));
}

void wolf3d_pokeb(uint16_t seg, uint16_t ofs, uint8_t value) {
    if (seg == SCREENSEG) {
        wolf3d_planar_store(ofs, 0, value);
    }
}

void wolf3d_poke(uint16_t seg, uint16_t ofs, uint16_t value) {
    wolf3d_pokeb(seg, ofs, (uint8_t)(value & 0xffu));
    wolf3d_pokeb(seg, (uint16_t)(ofs + 1u), (uint8_t)(value >> 8));
}

static unsigned int wolf3d_page_from_offset(unsigned offset) {
    if (offset >= WOLF3D_PAGE3START) {
        return 2u;
    }
    if (offset >= WOLF3D_PAGE2START) {
        return 1u;
    }
    return 0u;
}

static unsigned int wolf3d_page_base(unsigned int page) {
    if (page == 2u) {
        return WOLF3D_PAGE3START;
    }
    if (page == 1u) {
        return WOLF3D_PAGE2START;
    }
    return WOLF3D_PAGE1START;
}

static int wolf3d_offset_to_page(unsigned int offset, unsigned int* page,
                                 unsigned int* within) {
    unsigned int candidate = wolf3d_page_from_offset(offset);
    unsigned int base = wolf3d_page_base(candidate);

    if (offset >= WOLF3D_PAGE_BYTES * WOLF3D_VIDEO_PAGES) {
        return 0;
    }
    if (page) {
        *page = candidate;
    }
    if (within) {
        *within = offset - base;
    }
    return 1;
}

static void wolf3d_planar_store(unsigned int offset, unsigned int plane,
                                byte color) {
    unsigned int page;
    unsigned int within;
    unsigned int xbyte;
    unsigned int y;

    if (plane >= 4u || offset >= WOLF3D_VGA_BYTES) {
        return;
    }
    wolf3d_video_planes[plane][offset] = color;
    if (!wolf3d_offset_to_page(offset, &page, &within)) {
        return;
    }
    y = within / WOLF3D_VISIBLE_STRIDE;
    xbyte = within % WOLF3D_VISIBLE_STRIDE;
    if (y >= WOLF3D_SCREEN_H) {
        return;
    }
    wolf3d_video_pages[page][y * WOLF3D_SCREEN_W + xbyte * 4u + plane] = color;
    wolf3d_video_dirty = 1;
}

static byte wolf3d_planar_load(unsigned int offset, unsigned int plane) {
    if (plane >= 4u || offset >= WOLF3D_VGA_BYTES) {
        return 0;
    }
    return wolf3d_video_planes[plane][offset];
}

static byte wolf3d_palette_channel(int value) {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return (byte)value;
}

static uint32_t wolf3d_now_units(void) {
    return (uint32_t)wolf3d_syscall0(WOLF3D_GET_TICKS) *
           WOLF3D_TIMER_UNITS_PER_SMALLOS_TIC;
}

static uint32_t wolf3d_ticks_to_units(longword ticks) {
    return (uint32_t)ticks * WOLF3D_TIMER_UNITS_PER_TIC;
}

static longword wolf3d_units_to_ticks(uint32_t units) {
    return (longword)(units / WOLF3D_TIMER_UNITS_PER_TIC);
}

static void wolf3d_rebase_time_count(uint32_t now) {
    uint32_t elapsed = wolf3d_ticks_to_units(TimeCount);

    wolf3d_timer_base_units = now - elapsed;
    wolf3d_timer_observed = TimeCount;
    wolf3d_timer_ready = 1;
}

void wolf3d_sync_time_count(void) {
    uint32_t now = wolf3d_now_units();
    longword elapsed_ticks;

    if (!wolf3d_timer_ready || TimeCount != wolf3d_timer_observed) {
        wolf3d_rebase_time_count(now);
        wolf3d_sound_service(now);
        return;
    }
    elapsed_ticks = wolf3d_units_to_ticks(now - wolf3d_timer_base_units);
    if (elapsed_ticks > TimeCount) {
        TimeCount = elapsed_ticks;
    }
    wolf3d_timer_observed = TimeCount;
    wolf3d_sound_service(now);
}

static int wolf3d_pc_sound_is_playing(void) {
    return wolf3d_pc_sound_data &&
           TimeCount < wolf3d_pc_sound_end_tick;
}

static int wolf3d_digi_is_playing(void) {
    return DigiPlaying;
}

static word wolf3d_read_u16(const byte* data) {
    return (word)data[0] | (word)((word)data[1] << 8);
}

static longword wolf3d_read_u32(const byte* data) {
    return (longword)data[0] |
           ((longword)data[1] << 8) |
           ((longword)data[2] << 16) |
           ((longword)data[3] << 24);
}

static longword wolf3d_sound_chunk_length(const void* sound) {
    const byte* data = (const byte*)sound;

    return wolf3d_read_u32(data + WOLF3D_SOUND_COMMON_LENGTH);
}

static word wolf3d_sound_chunk_priority(const void* sound) {
    const byte* data = (const byte*)sound;

    return wolf3d_read_u16(data + WOLF3D_SOUND_COMMON_PRIORITY);
}

static const byte* wolf3d_pc_sound_chunk_data(const void* sound) {
    return (const byte*)sound + WOLF3D_PC_SOUND_DATA;
}

static void wolf3d_adlib_sound_chunk_inst(const void* sound, Instrument* inst) {
    const byte* data = (const byte*)sound + WOLF3D_AL_SOUND_INST;

    memcpy(inst, data, sizeof(*inst));
}

static byte wolf3d_adlib_sound_chunk_block(const void* sound) {
    return *((const byte*)sound + WOLF3D_AL_SOUND_BLOCK);
}

static byte* wolf3d_adlib_sound_chunk_data(void* sound) {
    return (byte*)sound + WOLF3D_AL_SOUND_DATA;
}

static int wolf3d_adlib_out(unsigned int reg, unsigned int value) {
    if (!AdLibPresent) {
        return -1;
    }
    return wolf3d_sound_op(SYS_SOUND_OP_OPL_WRITE, reg, value);
}

static void wolf3d_adlib_set_fx_inst(const Instrument* inst) {
    byte m = wolf3d_al_modifiers[0];
    byte c = wolf3d_al_carriers[0];

    if (!inst || !AdLibPresent) {
        return;
    }
    (void)wolf3d_adlib_out(m + WOLF3D_AL_CHAR, inst->mChar);
    (void)wolf3d_adlib_out(m + WOLF3D_AL_SCALE, inst->mScale);
    (void)wolf3d_adlib_out(m + WOLF3D_AL_ATTACK, inst->mAttack);
    (void)wolf3d_adlib_out(m + WOLF3D_AL_SUS, inst->mSus);
    (void)wolf3d_adlib_out(m + WOLF3D_AL_WAVE, inst->mWave);
    (void)wolf3d_adlib_out(c + WOLF3D_AL_CHAR, inst->cChar);
    (void)wolf3d_adlib_out(c + WOLF3D_AL_SCALE, inst->cScale);
    (void)wolf3d_adlib_out(c + WOLF3D_AL_ATTACK, inst->cAttack);
    (void)wolf3d_adlib_out(c + WOLF3D_AL_SUS, inst->cSus);
    (void)wolf3d_adlib_out(c + WOLF3D_AL_WAVE, inst->cWave);
    (void)wolf3d_adlib_out(WOLF3D_AL_FEED_CON, 0u);
}

static void wolf3d_adlib_stop_sound(void) {
    wolf3d_al_sound_data = NULL;
    wolf3d_al_sound_length = 0;
    wolf3d_al_sound_next_units = 0;
    wolf3d_al_sound_number = 0;
    wolf3d_al_sound_priority = 0;
    if (AdLibPresent) {
        (void)wolf3d_adlib_out(WOLF3D_AL_FREQ_H, 0u);
    }
}

static void wolf3d_adlib_music_silence(void) {
    wolf3d_music_active = 0;
    wolf3d_music_next_units = 0;
    if (AdLibPresent) {
        (void)wolf3d_adlib_out(WOLF3D_AL_EFFECTS, 0u);
        for (unsigned int i = 0; i < sqMaxTracks; i++) {
            (void)wolf3d_adlib_out(WOLF3D_AL_FREQ_H + i + 1u, 0u);
        }
    }
}

static void wolf3d_adlib_music_clear(void) {
    wolf3d_adlib_music_silence();
    wolf3d_music_data = NULL;
    wolf3d_music_ptr = NULL;
    wolf3d_music_len_bytes = 0;
    wolf3d_music_left_bytes = 0;
}

static void wolf3d_adlib_reset(void) {
    wolf3d_adlib_stop_sound();
    wolf3d_adlib_music_clear();
    if (AdLibPresent) {
        (void)wolf3d_sound_op(SYS_SOUND_OP_OPL_RESET, 0u, 0u);
    }
}

static void wolf3d_stop_sound_effects(void) {
    wolf3d_pc_sound_data = NULL;
    wolf3d_pc_sound_length = 0;
    wolf3d_pc_sound_end_tick = 0;
    wolf3d_pc_sound_priority = 0;
    wolf3d_pc_sound_number = 0;
    DigiPlaying = false;
    wolf3d_digi_end_tick = 0;
    wolf3d_digi_end_units = 0;
    wolf3d_digi_priority = 0;
    wolf3d_digi_number = 0;
    wolf3d_adlib_stop_sound();
    (void)wolf3d_sound_op(SYS_SOUND_OP_STOP, 0u, 0u);
}

static int wolf3d_adlib_play_sound(soundnames sound, void* al_sound) {
    Instrument inst;
    longword length;
    word priority;

    if (!AdLibPresent || !al_sound) {
        return 0;
    }
    length = wolf3d_sound_chunk_length(al_sound);
    priority = wolf3d_sound_chunk_priority(al_sound);
    if (!length) {
        return 0;
    }
    if (wolf3d_al_sound_data && priority < wolf3d_al_sound_priority) {
        return 0;
    }
    wolf3d_adlib_sound_chunk_inst(al_sound, &inst);
    if (!(inst.mSus | inst.cSus)) {
        return 0;
    }

    wolf3d_adlib_stop_sound();
    wolf3d_adlib_set_fx_inst(&wolf3d_al_zero_inst);
    wolf3d_adlib_set_fx_inst(&inst);
    wolf3d_al_sound_data = wolf3d_adlib_sound_chunk_data(al_sound);
    wolf3d_al_sound_length = length;
    wolf3d_al_block =
        (byte)(((wolf3d_adlib_sound_chunk_block(al_sound) & 7u) << 2) | 0x20u);
    wolf3d_al_sound_next_units = wolf3d_now_units();
    wolf3d_al_sound_number = (word)sound;
    wolf3d_al_sound_priority = priority;
    return 1;
}

static void wolf3d_adlib_service_sound(uint32_t now_units) {
    unsigned int events = 0;

    while (wolf3d_al_sound_data && wolf3d_al_sound_length &&
           (int32_t)(now_units - wolf3d_al_sound_next_units) >= 0 &&
           events++ < WOLF3D_ADLIB_MAX_EVENTS_PER_POLL) {
        byte sample = *wolf3d_al_sound_data++;

        if (!sample) {
            (void)wolf3d_adlib_out(WOLF3D_AL_FREQ_H, 0u);
        } else {
            (void)wolf3d_adlib_out(WOLF3D_AL_FREQ_L, sample);
            (void)wolf3d_adlib_out(WOLF3D_AL_FREQ_H, wolf3d_al_block);
        }
        wolf3d_al_sound_length--;
        wolf3d_al_sound_next_units += WOLF3D_ADLIB_SOUND_UNIT;
    }

    if (wolf3d_al_sound_data && wolf3d_al_sound_length == 0) {
        wolf3d_adlib_stop_sound();
    }
}

static void wolf3d_adlib_service_music(uint32_t now_units) {
    unsigned int events = 0;

    while (wolf3d_music_active && wolf3d_music_left_bytes >= 4u &&
           (int32_t)(now_units - wolf3d_music_next_units) >= 0 &&
           events++ < WOLF3D_ADLIB_MAX_EVENTS_PER_POLL) {
        word reg_value = *wolf3d_music_ptr++;
        word delay = *wolf3d_music_ptr++;

        wolf3d_music_left_bytes -= 4u;
        (void)wolf3d_adlib_out(reg_value & 0xffu, reg_value >> 8);
        wolf3d_music_next_units += (uint32_t)delay * WOLF3D_ADLIB_MUSIC_UNIT;

        if (wolf3d_music_left_bytes < 4u && wolf3d_music_data &&
            wolf3d_music_len_bytes >= 4u) {
            wolf3d_music_ptr = wolf3d_music_data;
            wolf3d_music_left_bytes = wolf3d_music_len_bytes;
            if (delay == 0u) {
                wolf3d_music_next_units = now_units;
            }
        }
    }
}

static void wolf3d_sound_stop(void) {
    wolf3d_stop_sound_effects();
    wolf3d_adlib_reset();
}

static void wolf3d_sound_service(uint32_t now_units) {
    wolf3d_adlib_service_music(now_units);
    wolf3d_adlib_service_sound(now_units);
    if (wolf3d_pc_sound_data && !wolf3d_pc_sound_is_playing()) {
        wolf3d_pc_sound_data = NULL;
        wolf3d_pc_sound_length = 0;
        wolf3d_pc_sound_end_tick = 0;
        wolf3d_pc_sound_priority = 0;
        wolf3d_pc_sound_number = 0;
    }
    if (DigiPlaying && (int32_t)(now_units - wolf3d_digi_end_units) >= 0) {
        DigiPlaying = false;
        wolf3d_digi_end_tick = 0;
        wolf3d_digi_end_units = 0;
        wolf3d_digi_priority = 0;
        wolf3d_digi_number = 0;
    }
}

static void wolf3d_wait_tics(unsigned int ticks) {
    longword target;
    uint32_t target_units;

    wolf3d_video_flush_deferred_present();
    if (!ticks) {
        ticks = 1u;
    }
    wolf3d_sync_time_count();
    wolf3d_input_poll();
    target = TimeCount + (longword)ticks;
    target_units = wolf3d_timer_base_units + wolf3d_ticks_to_units(target);

    while (1) {
        uint32_t now = wolf3d_now_units();
        uint32_t remaining;
        uint32_t sleep_us;

        if ((int32_t)(target_units - now) <= 0) {
            break;
        }
        wolf3d_sound_service(now);
        wolf3d_input_poll();
        remaining = target_units - now;
        if (remaining > WOLF3D_TIMER_UNITS_PER_SECOND / 20u) {
            remaining = WOLF3D_TIMER_UNITS_PER_SECOND / 20u;
        }
        sleep_us = (remaining * 1000000u) / WOLF3D_TIMER_UNITS_PER_SECOND;
        if (!sleep_us) {
            sleep_us = 1u;
        }
        (void)usleep(sleep_us);
    }

    wolf3d_input_poll();
    TimeCount = target;
    wolf3d_timer_observed = TimeCount;
}

static void wolf3d_fade_wait_vbl(void) {
    wolf3d_wait_tics(1u);
}

static void wolf3d_video_copy_bytes(unsigned int source, unsigned int dest,
                                    unsigned int width_bytes,
                                    unsigned int height) {
    if (width_bytes > WOLF3D_VISIBLE_STRIDE) {
        width_bytes = WOLF3D_VISIBLE_STRIDE;
    }
    if (height > WOLF3D_SCREEN_H) {
        height = WOLF3D_SCREEN_H;
    }

    for (unsigned int row = 0; row < height; row++) {
        unsigned int srcrow = source + row * linewidth;
        unsigned int dstrow = dest + row * linewidth;

        for (unsigned int col = 0; col < width_bytes; col++) {
            for (unsigned int plane = 0; plane < 4u; plane++) {
                wolf3d_planar_store(dstrow + col, plane,
                                    wolf3d_planar_load(srcrow + col, plane));
            }
        }
    }
}

void wolf3d_vga_write_planar_byte(void* base, unsigned int byte_offset,
                                  unsigned int plane, uint8_t value) {
    byte* ptr = (byte*)base;
    uintptr_t raw;

    if (!ptr) {
        return;
    }
    if (ptr < &wolf3d_video_planes[0][0] ||
        ptr >= &wolf3d_video_planes[0][WOLF3D_VGA_BYTES]) {
        ptr[byte_offset] = value;
        return;
    }
    raw = (uintptr_t)(ptr - &wolf3d_video_planes[0][0]) + byte_offset;
    wolf3d_planar_store((unsigned int)raw, plane, value);
}

static unsigned int wolf3d_dac_to_8(byte value) {
    if (value > 63u) {
        return value;
    }
    return ((unsigned int)value * 255u + 31u) / 63u;
}

static unsigned int wolf3d_palette_color(byte index) {
    unsigned int base = (unsigned int)index * 3u;
    unsigned int r = wolf3d_dac_to_8(wolf3d_palette[base + 0u]);
    unsigned int g = wolf3d_dac_to_8(wolf3d_palette[base + 1u]);
    unsigned int b = wolf3d_dac_to_8(wolf3d_palette[base + 2u]);

    return (r << 16) | (g << 8) | b;
}

static void wolf3d_video_rebuild_page(unsigned int page) {
    unsigned int base;

    if (page >= WOLF3D_VIDEO_PAGES) {
        return;
    }
    base = wolf3d_page_base(page);
    for (unsigned int y = 0; y < WOLF3D_SCREEN_H; y++) {
        for (unsigned int xbyte = 0; xbyte < WOLF3D_VISIBLE_STRIDE; xbyte++) {
            unsigned int offset = base + y * WOLF3D_VISIBLE_STRIDE + xbyte;

            for (unsigned int plane = 0; plane < 4u; plane++) {
                wolf3d_video_pages[page][y * WOLF3D_SCREEN_W + xbyte * 4u + plane] =
                    wolf3d_planar_load(offset, plane);
            }
        }
    }
}

static void wolf3d_video_put_offset(unsigned int base, int x, int y, byte color) {
    unsigned int offset;
    unsigned int plane;

    if (x < 0 || y < 0 ||
        x >= (int)WOLF3D_SCREEN_W || y >= (int)WOLF3D_SCREEN_H) {
        return;
    }
    offset = base + (unsigned int)y * WOLF3D_VISIBLE_STRIDE +
             (unsigned int)x / 4u;
    plane = (unsigned int)x & 3u;
    wolf3d_planar_store(offset, plane, color);
}

static void wolf3d_video_put(int x, int y, byte color) {
    wolf3d_video_put_offset(bufferofs, x, y, color);
    wolf3d_video_dirty = 1;
}

void wolf3d_set_post_source(const void* source, unsigned int offset) {
    wolf3d_post_source = (const byte*)source;
    postsource = (long)(uintptr_t)(offset & 0xffffu);
}

void wolf3d_scale_post(unsigned int x, unsigned int width,
                       unsigned int wall_height) {
    unsigned int scale;
    unsigned int draw_height;
    int top;
    int bottom;

    if (!wolf3d_post_source || !width || x >= (unsigned int)viewwidth ||
        maxscale < 1) {
        return;
    }
    if (x + width > (unsigned int)viewwidth) {
        width = (unsigned int)viewwidth - x;
    }
    scale = (wall_height & 0xfff8u) >> 3;
    if (scale > (unsigned int)maxscale) {
        scale = (unsigned int)maxscale;
    }
    if (!scale) {
        scale = 1;
    }
    draw_height = scale * 2u;
    top = ((int)viewheight - (int)draw_height) / 2;
    bottom = top + (int)draw_height;
    if (top < 0) {
        top = 0;
    }
    if (bottom > viewheight) {
        bottom = viewheight;
    }

    for (unsigned int sx = x; sx < x + width; sx++) {
        for (int y = top; y < bottom; y++) {
            int src = ((y - (((int)viewheight - (int)draw_height) / 2)) * 64) /
                      (int)draw_height;
            unsigned int texture_offset = (unsigned int)postsource & 0xfc0u;

            if (src < 0) {
                src = 0;
            } else if (src > 63) {
                src = 63;
            }
            wolf3d_video_put((int)sx, y,
                             wolf3d_post_source[texture_offset + (unsigned)src]);
        }
    }
}

void wolf3d_set_line_scale(const void* scale) {
    wolf3d_line_scale = (const t_compscale*)scale;
}

void wolf3d_set_line_shape(const void* shape) {
    wolf3d_line_shape = (const byte*)shape;
}

void wolf3d_scale_line(void) {
    const uint16_t* cmd = (const uint16_t*)linecmds;
    int yofs[65];
    int total_height = 0;
    int top;
    int draw_x = slinex;
    int draw_width = slinewidth;

    if (!wolf3d_line_scale || !wolf3d_line_shape || !cmd ||
        draw_width <= 0 || draw_x >= viewwidth) {
        return;
    }
    if (draw_x < 0) {
        draw_width += draw_x;
        draw_x = 0;
    }
    if (draw_x + draw_width > viewwidth) {
        draw_width = viewwidth - draw_x;
    }
    if (draw_width <= 0) {
        return;
    }

    for (int i = 0; i < 64; i++) {
        total_height += wolf3d_line_scale->width[i];
    }
    top = (viewheight - total_height) / 2;
    yofs[0] = top;
    for (int i = 0; i < 64; i++) {
        yofs[i + 1] = yofs[i] + wolf3d_line_scale->width[i];
    }

    while (cmd[0]) {
        unsigned int end = cmd[0] >> 1;
        unsigned int start = cmd[2] >> 1;
        const byte* source = wolf3d_line_shape + cmd[1];

        if (start > 64u) {
            start = 64u;
        }
        if (end > 64u) {
            end = 64u;
        }
        for (unsigned int src = start; src < end; src++) {
            int y0 = yofs[src];
            int y1 = yofs[src + 1];
            byte color = source[src];

            if (y0 < 0) {
                y0 = 0;
            }
            if (y1 > viewheight) {
                y1 = viewheight;
            }
            for (int y = y0; y < y1; y++) {
                for (int x = 0; x < draw_width; x++) {
                    wolf3d_video_put(draw_x + x, y, color);
                }
            }
        }
        cmd += 3;
    }
}

static void wolf3d_display_clear(void) {
    wolf3d_display_fill_rect_t rect;

    if (!wolf3d_display_acquired) {
        return;
    }
    rect.x = 0;
    rect.y = 0;
    rect.w = wolf3d_display.width;
    rect.h = wolf3d_display.height;
    rect.color = 0xff000000u;
    (void)wolf3d_syscall1(WOLF3D_DISPLAY_FILL, (unsigned int)&rect);
    wolf3d_display_cleared = 1;
}

static unsigned int wolf3d_display_scale(void) {
    unsigned int scale_x = wolf3d_display.width / WOLF3D_SCREEN_W;
    unsigned int scale_y = wolf3d_display.height / WOLF3D_SCREEN_H;
    unsigned int scale = scale_x < scale_y ? scale_x : scale_y;

    return scale ? scale : 1u;
}

static int wolf3d_display_scale_buffer(unsigned int page, unsigned int scale,
                                       unsigned int* out_w,
                                       unsigned int* out_h,
                                       unsigned int** out_pixels) {
    unsigned int scaled_w = WOLF3D_SCREEN_W * scale;
    unsigned int scaled_h = WOLF3D_SCREEN_H * scale;
    unsigned int needed = scaled_w * scaled_h;

    if (scale == 1u) {
        *out_w = WOLF3D_SCREEN_W;
        *out_h = WOLF3D_SCREEN_H;
        *out_pixels = wolf3d_xrgb;
        for (unsigned int i = 0; i < WOLF3D_SCREEN_PIXELS; i++) {
            wolf3d_xrgb[i] = wolf3d_palette_color(wolf3d_video_pages[page][i]);
        }
        return 1;
    }

    if (needed > wolf3d_scaled_capacity) {
        unsigned int* scaled = realloc(wolf3d_scaled_xrgb,
                                       needed * sizeof(*wolf3d_scaled_xrgb));
        if (!scaled) {
            return 0;
        }
        wolf3d_scaled_xrgb = scaled;
        wolf3d_scaled_capacity = needed;
    }

    for (unsigned int src_y = 0; src_y < WOLF3D_SCREEN_H; src_y++) {
        unsigned int* row = wolf3d_scaled_xrgb + src_y * scale * scaled_w;
        for (unsigned int src_x = 0; src_x < WOLF3D_SCREEN_W; src_x++) {
            unsigned int color =
                wolf3d_palette_color(wolf3d_video_pages[page]
                                     [src_y * WOLF3D_SCREEN_W + src_x]);
            for (unsigned int sx = 0; sx < scale; sx++) {
                row[src_x * scale + sx] = color;
            }
        }
        for (unsigned int sy = 1; sy < scale; sy++) {
            memcpy(row + sy * scaled_w, row, scaled_w * sizeof(*row));
        }
    }

    *out_w = scaled_w;
    *out_h = scaled_h;
    *out_pixels = wolf3d_scaled_xrgb;
    return 1;
}

static int wolf3d_display_present_mapped(unsigned int page, unsigned int scale,
                                         unsigned int present_w,
                                         unsigned int present_h,
                                         unsigned int x, unsigned int y) {
    volatile unsigned char* page_base;
    wolf3d_display_present_page_t req;
    unsigned int draw_page;
    unsigned int bottom;
    unsigned int row_end;

    if (!wolf3d_display_mapped ||
        page >= WOLF3D_VIDEO_PAGES ||
        scale == 0u ||
        present_w == 0u ||
        present_h == 0u ||
        x > wolf3d_display_map.width ||
        y > wolf3d_display_map.height ||
        present_w > wolf3d_display_map.width - x ||
        present_h > wolf3d_display_map.height - y ||
        wolf3d_display_map.draw_page >= wolf3d_display_map.page_count) {
        return 0;
    }

    bottom = y + present_h - 1u;
    row_end = bottom * wolf3d_display_map.pitch + (x + present_w) * 4u;
    if (row_end > wolf3d_display_map.page_bytes) {
        return 0;
    }

    draw_page = wolf3d_display_map.draw_page;
    page_base = (volatile unsigned char*)(uintptr_t)
        (wolf3d_display_map.base + draw_page * wolf3d_display_map.page_bytes);

    for (unsigned int src_y = 0; src_y < WOLF3D_SCREEN_H; src_y++) {
        volatile unsigned int* row =
            (volatile unsigned int*)(page_base +
                (y + src_y * scale) * wolf3d_display_map.pitch + x * 4u);

        for (unsigned int src_x = 0; src_x < WOLF3D_SCREEN_W; src_x++) {
            unsigned int color =
                wolf3d_palette_color(wolf3d_video_pages[page]
                                     [src_y * WOLF3D_SCREEN_W + src_x]);
            for (unsigned int sx = 0; sx < scale; sx++) {
                row[src_x * scale + sx] = color;
            }
        }
        for (unsigned int sy = 1; sy < scale; sy++) {
            volatile unsigned int* dup =
                (volatile unsigned int*)((volatile unsigned char*)row +
                                         sy * wolf3d_display_map.pitch);
            for (unsigned int px = 0; px < present_w; px++) {
                dup[px] = row[px];
            }
        }
    }

    req.page = draw_page;
    req.next_page = draw_page;
    if (wolf3d_syscall1(WOLF3D_DISPLAY_PRESENT_PAGE,
                        (unsigned int)&req) < 0 ||
        req.next_page >= wolf3d_display_map.page_count) {
        wolf3d_display_mapped = 0;
        return 0;
    }
    wolf3d_display_map.draw_page = req.next_page;
    wolf3d_video_dirty = 0;
    wolf3d_video_deferred_present = 0;
    return 1;
}

static int wolf3d_display_open(void) {
    if (wolf3d_display_acquired) {
        return 1;
    }
    if (wolf3d_display_checked) {
        return 0;
    }
    wolf3d_display_checked = 1;
    memset(&wolf3d_display, 0, sizeof(wolf3d_display));
    if (wolf3d_syscall1(WOLF3D_DISPLAY_INFO, (unsigned int)&wolf3d_display) < 0 ||
        wolf3d_display.format != WOLF3D_DISPLAY_FORMAT_XRGB8888 ||
        wolf3d_display.bpp != 32u ||
        wolf3d_display.width < WOLF3D_SCREEN_W ||
        wolf3d_display.height < WOLF3D_SCREEN_H) {
        return 0;
    }
    if (wolf3d_syscall0(WOLF3D_DISPLAY_ACQUIRE) < 0) {
        return 0;
    }
    wolf3d_display_acquired = 1;
    memset(&wolf3d_display_map, 0, sizeof(wolf3d_display_map));
    wolf3d_display_mapped =
        wolf3d_syscall1(WOLF3D_DISPLAY_MAP,
                        (unsigned int)&wolf3d_display_map) >= 0 &&
        wolf3d_display_map.format == WOLF3D_DISPLAY_FORMAT_XRGB8888 &&
        wolf3d_display_map.bpp == 32u &&
        wolf3d_display_map.width == wolf3d_display.width &&
        wolf3d_display_map.height == wolf3d_display.height &&
        wolf3d_display_map.pitch == wolf3d_display.pitch &&
        wolf3d_display_map.page_count >= 2u &&
        wolf3d_display_map.draw_page < wolf3d_display_map.page_count;
    wolf3d_display_cleared = 0;
    wolf3d_display_clear();
    return 1;
}

static void wolf3d_video_present_page(unsigned int page) {
    wolf3d_display_blit_rect_t rect;
    unsigned int x;
    unsigned int y;
    unsigned int scale;
    unsigned int present_w;
    unsigned int present_h;
    unsigned int* present_pixels;

    if (page >= WOLF3D_VIDEO_PAGES || !wolf3d_display_open()) {
        return;
    }
    wolf3d_sound_service(wolf3d_now_units());
    if (!wolf3d_display_cleared) {
        wolf3d_display_clear();
    }
    wolf3d_video_rebuild_page(page);
    scale = wolf3d_display_scale();
    present_w = WOLF3D_SCREEN_W * scale;
    present_h = WOLF3D_SCREEN_H * scale;
    x = (wolf3d_display.width - present_w) / 2u;
    y = (wolf3d_display.height - present_h) / 2u;
    if (wolf3d_display_present_mapped(page, scale, present_w, present_h,
                                      x, y)) {
        return;
    }

    if (!wolf3d_display_scale_buffer(page, scale, &present_w, &present_h,
                                     &present_pixels)) {
        scale = 1u;
        (void)wolf3d_display_scale_buffer(page, scale, &present_w, &present_h,
                                          &present_pixels);
    }
    x = (wolf3d_display.width - present_w) / 2u;
    y = (wolf3d_display.height - present_h) / 2u;
    rect.x = x;
    rect.y = y;
    rect.w = present_w;
    rect.h = present_h;
    rect.pixels = present_pixels;
    (void)wolf3d_syscall1(WOLF3D_DISPLAY_BLIT, (unsigned int)&rect);
    wolf3d_video_dirty = 0;
    wolf3d_video_deferred_present = 0;
}

static void wolf3d_video_present_offset_if_visible(unsigned int offset) {
    unsigned int visible_page;
    unsigned int draw_page;

    if (displayofs >= WOLF3D_PAGE_BYTES * WOLF3D_VIDEO_PAGES) {
        return;
    }
    visible_page = wolf3d_page_from_offset(displayofs);
    draw_page = wolf3d_page_from_offset(offset);
    if (draw_page == visible_page) {
        wolf3d_video_deferred_present = 1;
        wolf3d_video_dirty = 1;
    }
}

static void wolf3d_video_flush_deferred_present(void) {
    if (wolf3d_video_deferred_present) {
        wolf3d_video_deferred_present = 0;
        wolf3d_video_present();
    }
}

static void wolf3d_video_present(void) {
    unsigned int page = wolf3d_page_from_offset(displayofs);

    if (displayofs >= WOLF3D_PAGE_BYTES * WOLF3D_VIDEO_PAGES) {
        page = wolf3d_page_from_offset(bufferofs);
    }
    wolf3d_video_present_page(page);
}

static void wolf3d_video_commit_buffer(void) {
    if (bufferofs != displayofs) {
        wolf3d_video_copy_bytes(bufferofs, displayofs, WOLF3D_VISIBLE_STRIDE,
                                WOLF3D_SCREEN_H);
    }
    wolf3d_video_present_page(wolf3d_page_from_offset(displayofs));
}

int CheckIs386(void) {
    return 1;
}

void jabhack2(void) {
}

static int wolf3d_mouse_clamp_delta(int value) {
    if (value > WOLF3D_MOUSE_MAX_DELTA) {
        return WOLF3D_MOUSE_MAX_DELTA;
    }
    if (value < WOLF3D_MOUSE_MIN_DELTA) {
        return WOLF3D_MOUSE_MIN_DELTA;
    }
    return value;
}

void wolf3d_geninterrupt(unsigned int intno) {
    unsigned int ax;

    if (intno != WOLF3D_MOUSE_INT) {
        return;
    }

    ax = wolf3d_reg_ax & 0xffffu;
    wolf3d_input_poll();

    switch (ax) {
        case WOLF3D_MOUSE_RESET:
            wolf3d_reg_ax = 0xffffu;
            wolf3d_reg_bx = 3u;
            wolf3d_reg_cx = 0;
            wolf3d_reg_dx = 0;
            wolf3d_mouse_dx = 0;
            wolf3d_mouse_dy = 0;
            wolf3d_mouse_x = WOLF3D_MOUSE_CENTER;
            wolf3d_mouse_y = WOLF3D_MOUSE_CENTER;
            break;
        case WOLF3D_MOUSE_BUTTONS:
            wolf3d_reg_bx = wolf3d_mouse_buttons & 0x7u;
            wolf3d_reg_cx = (unsigned int)wolf3d_mouse_x;
            wolf3d_reg_dx = (unsigned int)wolf3d_mouse_y;
            break;
        case WOLF3D_MOUSE_SET_POSITION:
            wolf3d_mouse_x = (int)wolf3d_reg_cx;
            wolf3d_mouse_y = (int)wolf3d_reg_dx;
            wolf3d_mouse_dx = 0;
            wolf3d_mouse_dy = 0;
            break;
        case WOLF3D_MOUSE_DELTA:
        {
            int dx = wolf3d_mouse_clamp_delta(wolf3d_mouse_dx);
            int dy = wolf3d_mouse_clamp_delta(wolf3d_mouse_dy);

            wolf3d_reg_bx = wolf3d_mouse_buttons & 0x7u;
            wolf3d_reg_cx = (unsigned int)dx;
            wolf3d_reg_dx = (unsigned int)dy;
            wolf3d_mouse_dx = 0;
            wolf3d_mouse_dy = 0;
            break;
        }
        default:
            break;
    }
}

void US_InitRndT(boolean randomize) {
    wolf3d_rnd_state = randomize ? (unsigned)(TimeCount | 1u) : 1u;
}

int US_RndT(void) {
    wolf3d_rnd_state = wolf3d_rnd_state * 1103515245u + 12345u;
    return (int)((wolf3d_rnd_state >> 16) & 0xffu);
}

static ScanCode wolf3d_ascii_scan(unsigned int ascii) {
    switch (ascii) {
        case 8: return sc_BackSpace;
        case 9: return sc_Tab;
        case 10:
        case 13: return sc_Enter;
        case 27: return sc_Escape;
        case ' ': return sc_Space;
        case '!': return sc_1;
        case '@': return sc_2;
        case '#': return sc_3;
        case '$': return sc_4;
        case '%': return sc_5;
        case '^': return sc_6;
        case '&': return sc_7;
        case '*': return sc_8;
        case '(': return sc_9;
        case ')': return sc_0;
        case '0': return sc_0;
        case '1': return sc_1;
        case '2': return sc_2;
        case '3': return sc_3;
        case '4': return sc_4;
        case '5': return sc_5;
        case '6': return sc_6;
        case '7': return sc_7;
        case '8': return sc_8;
        case '9': return sc_9;
        case 'a':
        case 'A': return sc_A;
        case 'b':
        case 'B': return sc_B;
        case 'c':
        case 'C': return sc_C;
        case 'd':
        case 'D': return sc_D;
        case 'e':
        case 'E': return sc_E;
        case 'f':
        case 'F': return sc_F;
        case 'g':
        case 'G': return sc_G;
        case 'h':
        case 'H': return sc_H;
        case 'i':
        case 'I': return sc_I;
        case 'j':
        case 'J': return sc_J;
        case 'k':
        case 'K': return sc_K;
        case 'l':
        case 'L': return sc_L;
        case 'm':
        case 'M': return sc_M;
        case 'n':
        case 'N': return sc_N;
        case 'o':
        case 'O': return sc_O;
        case 'p':
        case 'P': return sc_P;
        case 'q':
        case 'Q': return sc_Q;
        case 'r':
        case 'R': return sc_R;
        case 's':
        case 'S': return sc_S;
        case 't':
        case 'T': return sc_T;
        case 'u':
        case 'U': return sc_U;
        case 'v':
        case 'V': return sc_V;
        case 'w':
        case 'W': return sc_W;
        case 'x':
        case 'X': return sc_X;
        case 'y':
        case 'Y': return sc_Y;
        case 'z':
        case 'Z': return sc_Z;
        case '-':
        case '_': return (ScanCode)0x0c;
        case '=':
        case '+': return (ScanCode)0x0d;
        case '[':
        case '{': return (ScanCode)0x1a;
        case ']':
        case '}': return (ScanCode)0x1b;
        case ';':
        case ':': return (ScanCode)0x27;
        case '\'':
        case '"': return (ScanCode)0x28;
        case '`':
        case '~': return (ScanCode)0x29;
        case '\\':
        case '|': return (ScanCode)0x2b;
        case ',':
        case '<': return (ScanCode)0x33;
        case '.':
        case '>': return (ScanCode)0x34;
        case '/':
        case '?': return (ScanCode)0x35;
        default: return sc_None;
    }
}

static ScanCode wolf3d_key_scan(unsigned int key) {
    switch ((keycode_t)key) {
        case KEY_ESC: return sc_Escape;
        case KEY_ENTER:
        case KEY_KP_ENTER: return sc_Enter;
        case KEY_1: return sc_1;
        case KEY_2: return sc_2;
        case KEY_3: return sc_3;
        case KEY_4: return sc_4;
        case KEY_5: return sc_5;
        case KEY_6: return sc_6;
        case KEY_7: return sc_7;
        case KEY_8: return sc_8;
        case KEY_9: return sc_9;
        case KEY_0: return sc_0;
        case KEY_A: return sc_A;
        case KEY_B: return sc_B;
        case KEY_C: return sc_C;
        case KEY_D: return sc_D;
        case KEY_E: return sc_E;
        case KEY_F: return sc_F;
        case KEY_G: return sc_G;
        case KEY_H: return sc_H;
        case KEY_I: return sc_I;
        case KEY_J: return sc_J;
        case KEY_K: return sc_K;
        case KEY_L: return sc_L;
        case KEY_M: return sc_M;
        case KEY_N: return sc_N;
        case KEY_O: return sc_O;
        case KEY_P: return sc_P;
        case KEY_Q: return sc_Q;
        case KEY_R: return sc_R;
        case KEY_S: return sc_S;
        case KEY_T: return sc_T;
        case KEY_U: return sc_U;
        case KEY_V: return sc_V;
        case KEY_W: return sc_W;
        case KEY_X: return sc_X;
        case KEY_Y: return sc_Y;
        case KEY_Z: return sc_Z;
        case KEY_MINUS: return (ScanCode)0x0c;
        case KEY_EQUALS: return (ScanCode)0x0d;
        case KEY_LBRACKET: return (ScanCode)0x1a;
        case KEY_RBRACKET: return (ScanCode)0x1b;
        case KEY_SEMICOLON: return (ScanCode)0x27;
        case KEY_APOSTROPHE: return (ScanCode)0x28;
        case KEY_GRAVE: return (ScanCode)0x29;
        case KEY_BACKSLASH: return (ScanCode)0x2b;
        case KEY_COMMA: return (ScanCode)0x33;
        case KEY_DOT: return (ScanCode)0x34;
        case KEY_SLASH: return (ScanCode)0x35;
        case KEY_SPACE: return sc_Space;
        case KEY_BACKSPACE: return sc_BackSpace;
        case KEY_TAB: return sc_Tab;
        case KEY_HOME:
        case KEY_KP7: return sc_Home;
        case KEY_UP:
        case KEY_KP8: return sc_UpArrow;
        case KEY_PAGEUP:
        case KEY_KP9: return sc_PgUp;
        case KEY_DOWN:
        case KEY_KP2: return sc_DownArrow;
        case KEY_LEFT:
        case KEY_KP4: return sc_LeftArrow;
        case KEY_RIGHT:
        case KEY_KP6: return sc_RightArrow;
        case KEY_END:
        case KEY_KP1: return sc_End;
        case KEY_PAGEDOWN:
        case KEY_KP3: return sc_PgDn;
        case KEY_INSERT:
        case KEY_KP0: return sc_Insert;
        case KEY_DELETE:
        case KEY_KP_DOT: return sc_Delete;
        case KEY_LSHIFT: return sc_LShift;
        case KEY_RSHIFT: return sc_RShift;
        case KEY_LCTRL:
        case KEY_RCTRL: return sc_Control;
        case KEY_LALT:
        case KEY_RALT: return sc_Alt;
        case KEY_CAPSLOCK: return sc_CapsLock;
        case KEY_NUMLOCK: return (ScanCode)0x45;
        case KEY_SCROLLLOCK: return (ScanCode)0x46;
        case KEY_KP_STAR:
        case KEY_PRINT_SCREEN: return (ScanCode)0x37;
        case KEY_KP_MINUS: return (ScanCode)0x4a;
        case KEY_KP_PLUS: return (ScanCode)0x4e;
        case KEY_KP5: return (ScanCode)0x4c;
        case KEY_KP_SLASH: return (ScanCode)0x35;
        case KEY_PAUSE: return (ScanCode)0x45;
        case KEY_F1: return sc_F1;
        case KEY_F2: return sc_F2;
        case KEY_F3: return sc_F3;
        case KEY_F4: return sc_F4;
        case KEY_F5: return sc_F5;
        case KEY_F6: return sc_F6;
        case KEY_F7: return sc_F7;
        case KEY_F8: return sc_F8;
        case KEY_F9: return sc_F9;
        case KEY_F10: return sc_F10;
        case KEY_F11: return sc_F11;
        case KEY_F12: return sc_F12;
        default: break;
    }
    return sc_None;
}

static char wolf3d_ascii_char(unsigned int ascii, unsigned int key) {
    if (ascii == 10 || key == KEY_ENTER || key == KEY_KP_ENTER) {
        return key_Enter;
    }
    if (key == KEY_ESC) {
        return key_Escape;
    }
    if (ascii > 0 && ascii < 128u) {
        return (char)ascii;
    }
    return key_None;
}

static void wolf3d_input_apply(const sys_input_event_t* ev) {
    ScanCode scan;
    int pressed;

    if (!ev) {
        return;
    }
    if (ev->type == SYS_INPUT_TYPE_MOUSE) {
        wolf3d_mouse_dx += ev->dx;
        wolf3d_mouse_dy += ev->dy;
        wolf3d_mouse_x += ev->dx;
        wolf3d_mouse_y += ev->dy;
        wolf3d_mouse_buttons = ev->buttons & 0x7u;
        return;
    }
    if (ev->type != SYS_INPUT_TYPE_KEY) {
        return;
    }
    scan = wolf3d_key_scan(ev->key);
    if (scan == sc_None) {
        scan = wolf3d_ascii_scan(ev->ascii);
    }
    if (scan == sc_None || scan >= NumCodes) {
        return;
    }
    pressed = (ev->flags & SYS_INPUT_KEY_PRESSED) != 0u;
    Keyboard[scan] = pressed ? true : false;
    if (pressed) {
        char ascii = wolf3d_ascii_char(ev->ascii, ev->key);

        LastScan = scan;
        if (ascii) {
            LastASCII = ascii;
        }
    }
}

void wolf3d_input_poll(void) {
    sys_input_event_t events[32];
    int count;

    wolf3d_sound_service(wolf3d_now_units());
    do {
        count = wolf3d_syscall3(WOLF3D_INPUT_READ, (unsigned int)events,
                                sizeof(events) / sizeof(events[0]),
                                SYS_INPUT_FLAG_NONBLOCK);
        if (count <= 0) {
            break;
        }
        for (int i = 0; i < count; i++) {
            wolf3d_input_apply(&events[i]);
        }
    } while (count == (int)(sizeof(events) / sizeof(events[0])));
}

static int wolf3d_valid_scan(int scan) {
    return scan > sc_None && scan < NumCodes;
}

static int wolf3d_valid_button_binding(int button) {
    return button >= bt_nobutton && button < NUMBUTTONS;
}

void wolf3d_reset_control_bindings(void) {
    dirscan[di_north] = sc_UpArrow;
    dirscan[di_east] = sc_RightArrow;
    dirscan[di_south] = sc_DownArrow;
    dirscan[di_west] = sc_LeftArrow;

    buttonscan[bt_attack] = sc_Control;
    buttonscan[bt_strafe] = sc_Alt;
    buttonscan[bt_run] = sc_RShift;
    buttonscan[bt_use] = sc_Space;
    buttonscan[bt_readyknife] = sc_1;
    buttonscan[bt_readypistol] = sc_2;
    buttonscan[bt_readymachinegun] = sc_3;
    buttonscan[bt_readychaingun] = sc_4;

    buttonmouse[0] = bt_attack;
    buttonmouse[1] = bt_strafe;
    buttonmouse[2] = bt_use;
    buttonmouse[3] = bt_nobutton;

    buttonjoy[0] = bt_attack;
    buttonjoy[1] = bt_strafe;
    buttonjoy[2] = bt_use;
    buttonjoy[3] = bt_run;
}

void wolf3d_validate_control_config(void) {
    int invalid = 0;

    for (int i = 0; i < 4; i++) {
        if (!wolf3d_valid_scan(dirscan[i])) {
            invalid = 1;
        }
        if (!wolf3d_valid_button_binding(buttonmouse[i]) ||
            !wolf3d_valid_button_binding(buttonjoy[i])) {
            invalid = 1;
        }
    }
    for (int i = 0; i < NUMBUTTONS; i++) {
        if (!wolf3d_valid_scan(buttonscan[i])) {
            invalid = 1;
        }
    }
    if (joystickport < 0 || joystickport >= MaxJoys) {
        joystickport = 0;
        joystickenabled = false;
    }
    if (mouseadjustment < 0 || mouseadjustment > 9) {
        mouseadjustment = 5;
    }
    if (viewsize < 4 || viewsize > 19) {
        viewsize = 15;
    }
    if (invalid) {
        wolf3d_reset_control_bindings();
    }
}

static void wolf3d_input_discard_pending(void) {
    sys_input_event_t events[8];

    while (wolf3d_syscall3(WOLF3D_INPUT_READ, (unsigned int)events,
                           sizeof(events) / sizeof(events[0]),
                           SYS_INPUT_FLAG_NONBLOCK) > 0) {
    }
}

void wolf3d_input_clear(void) {
    LastScan = sc_None;
    LastASCII = key_None;
    memset(Keyboard, 0, sizeof(boolean) * NumCodes);

    wolf3d_input_discard_pending();

    LastScan = sc_None;
    LastASCII = key_None;
    memset(Keyboard, 0, sizeof(boolean) * NumCodes);
}

void wolf3d_platform_shutdown(void) {
    wolf3d_input_clear();
    wolf3d_display_release();
}

__attribute__((noreturn)) void wolf3d_exit(int code) {
    wolf3d_platform_shutdown();
    (void)wolf3d_syscall1(WOLF3D_SYS_EXIT, (unsigned int)code);
    for (;;) {
    }
}

void wolf3d_vga_clear_view(void) {
    int half = viewheight / 2;
    byte ceiling = (byte)(vgaCeiling[gamestate.episode * 10 + mapon] & 0xffu);
    byte floor = 0x19;

    for (int y = 0; y < viewheight; y++) {
        byte color = y < half ? ceiling : floor;

        for (int x = 0; x < viewwidth; x++) {
            wolf3d_video_put(x, y, color);
        }
    }
}

static fixed wolf3d_fixed_by_partial(fixed value, unsigned int partial) {
    return (fixed)(((int64_t)value * (int64_t)(uint16_t)partial) >> 16);
}

static int wolf3d_fixed_tile(fixed value) {
    return (int)(value >> TILESHIFT);
}

static unsigned int wolf3d_fixed_frac(fixed value) {
    return (unsigned int)value & (TILEGLOBAL - 1u);
}

static fixed wolf3d_fixed_half(fixed value) {
    return value >> 1;
}

static int wolf3d_tile_in_bounds(int tx, int ty) {
    return tx >= 0 && ty >= 0 && tx < MAPSIZE && ty < MAPSIZE;
}

static void wolf3d_setup_ray(int rayangle, unsigned int* xpartial,
                             unsigned int* ypartial) {
    while (rayangle < 0) {
        rayangle += FINEANGLES;
    }
    while (rayangle >= FINEANGLES) {
        rayangle -= FINEANGLES;
    }

    if (rayangle < WOLF3D_DEG90) {
        xtilestep = 1;
        ytilestep = -1;
        xstep = finetangent[WOLF3D_DEG90 - 1 - rayangle];
        ystep = -finetangent[rayangle];
        *xpartial = xpartialup;
        *ypartial = ypartialdown;
    } else if (rayangle < WOLF3D_DEG180) {
        xtilestep = -1;
        ytilestep = -1;
        xstep = -finetangent[rayangle - WOLF3D_DEG90];
        ystep = -finetangent[WOLF3D_DEG180 - 1 - rayangle];
        *xpartial = xpartialdown;
        *ypartial = ypartialdown;
    } else if (rayangle < WOLF3D_DEG270) {
        xtilestep = -1;
        ytilestep = 1;
        xstep = -finetangent[WOLF3D_DEG270 - 1 - rayangle];
        ystep = finetangent[rayangle - WOLF3D_DEG180];
        *xpartial = xpartialdown;
        *ypartial = ypartialup;
    } else {
        xtilestep = 1;
        ytilestep = 1;
        xstep = finetangent[rayangle - WOLF3D_DEG270];
        ystep = finetangent[WOLF3D_DEG360 - 1 - rayangle];
        *xpartial = xpartialup;
        *ypartial = ypartialup;
    }
}

static int wolf3d_horiz_before_vert(int yinttile, int ray_ytile) {
    if (ytilestep < 0) {
        return yinttile <= ray_ytile;
    }
    return yinttile >= ray_ytile;
}

static int wolf3d_vert_before_horiz(int xinttile, int ray_xtile) {
    if (xtilestep < 0) {
        return xinttile <= ray_xtile;
    }
    return xinttile >= ray_xtile;
}

static int wolf3d_hit_horiz_special(int xinttile, int ray_ytile) {
    unsigned int doornum = tilehit & 0x7fu;
    fixed intercept;

    if (tilehit & 0x40u) {
        intercept = xintercept +
                    wolf3d_fixed_by_partial(xstep, (unsigned int)pwallpos << 10);
        if (wolf3d_fixed_tile(intercept) != xinttile) {
            return 0;
        }
        xintercept = intercept;
        yintercept = (fixed)ray_ytile << TILESHIFT;
        xtile = xinttile;
        ytile = ray_ytile;
        HitHorizPWall();
        return 1;
    }

    intercept = xintercept + wolf3d_fixed_half(xstep);
    if (wolf3d_fixed_tile(intercept) != xinttile) {
        return 0;
    }
    if (doornum < MAXDOORS &&
        wolf3d_fixed_frac(intercept) < doorposition[doornum]) {
        return 0;
    }
    xintercept = intercept;
    yintercept = ((fixed)ray_ytile << TILESHIFT) + 0x8000L;
    xtile = xinttile;
    ytile = ray_ytile;
    HitHorizDoor();
    return 1;
}

static int wolf3d_hit_vert_special(int ray_xtile, int yinttile) {
    unsigned int doornum = tilehit & 0x7fu;
    fixed intercept;

    if (tilehit & 0x40u) {
        intercept = yintercept +
                    wolf3d_fixed_by_partial(ystep, (unsigned int)pwallpos << 10);
        if (wolf3d_fixed_tile(intercept) != yinttile) {
            return 0;
        }
        yintercept = intercept;
        xintercept = (fixed)ray_xtile << TILESHIFT;
        xtile = ray_xtile;
        ytile = yinttile;
        HitVertPWall();
        return 1;
    }

    intercept = yintercept + wolf3d_fixed_half(ystep);
    if (wolf3d_fixed_tile(intercept) != yinttile) {
        return 0;
    }
    if (doornum < MAXDOORS &&
        wolf3d_fixed_frac(intercept) < doorposition[doornum]) {
        return 0;
    }
    yintercept = intercept;
    xintercept = ((fixed)ray_xtile << TILESHIFT) + 0x8000L;
    xtile = ray_xtile;
    ytile = yinttile;
    HitVertDoor();
    return 1;
}

void AsmRefresh(void) {
    int width = viewwidth;

    if (width > MAXVIEWWIDTH) {
        width = MAXVIEWWIDTH;
    }

    for (pixx = 0; pixx < (unsigned)width; pixx++) {
        unsigned int xpartial;
        unsigned int ypartial;
        int ray_xtile;
        int ray_ytile;
        int xinttile;
        int yinttile;
        int state = 0;

        wolf3d_setup_ray(midangle + pixelangle[pixx], &xpartial, &ypartial);

        yintercept = viewy + wolf3d_fixed_by_partial(ystep, xpartial);
        ray_xtile = focaltx + xtilestep;
        yinttile = wolf3d_fixed_tile(yintercept);

        xintercept = viewx + wolf3d_fixed_by_partial(xstep, ypartial);
        xinttile = wolf3d_fixed_tile(xintercept);
        ray_ytile = focalty + ytilestep;

        for (int trace = 0; trace < MAPSIZE * 4; trace++) {
            if (state == 0) {
                state = wolf3d_horiz_before_vert(yinttile, ray_ytile) ? 3 : 1;
                continue;
            }
            if (state == 2) {
                state = wolf3d_vert_before_horiz(xinttile, ray_xtile) ? 1 : 3;
                continue;
            }

            if (state == 1) {
                if (!wolf3d_tile_in_bounds(ray_xtile, yinttile)) {
                    break;
                }
                tilehit = tilemap[ray_xtile][yinttile];
                if (tilehit) {
                    if (tilehit & 0x80u) {
                        if (wolf3d_hit_vert_special(ray_xtile, yinttile)) {
                            break;
                        }
                    } else {
                        xintercept = (fixed)ray_xtile << TILESHIFT;
                        xtile = ray_xtile;
                        ytile = yinttile;
                        HitVertWall();
                        break;
                    }
                }
                spotvis[ray_xtile][yinttile] = true;
                ray_xtile += xtilestep;
                yintercept += ystep;
                yinttile = wolf3d_fixed_tile(yintercept);
                state = 0;
                continue;
            }

            if (!wolf3d_tile_in_bounds(xinttile, ray_ytile)) {
                break;
            }
            tilehit = tilemap[xinttile][ray_ytile];
            if (tilehit) {
                if (tilehit & 0x80u) {
                    if (wolf3d_hit_horiz_special(xinttile, ray_ytile)) {
                        break;
                    }
                } else {
                    yintercept = (fixed)ray_ytile << TILESHIFT;
                    xtile = xinttile;
                    ytile = ray_ytile;
                    HitHorizWall();
                    break;
                }
            }
            spotvis[xinttile][ray_ytile] = true;
            ray_ytile += ytilestep;
            xintercept += xstep;
            xinttile = wolf3d_fixed_tile(xintercept);
            state = 2;
        }
    }
}

void MM_Startup(void) {
    memcpy(&signon, smallos_wolf3d_signon, sizeof(smallos_wolf3d_signon));
    memcpy(&gamepal, smallos_wolf3d_palette, sizeof(smallos_wolf3d_palette));
    mminfo.nearheap = WOLF3D_DOS_NEAR_HEAP_BYTES;
    mminfo.farheap = WOLF3D_DOS_FAR_HEAP_BYTES;
    mminfo.EMSmem = 0;
    mminfo.XMSmem = 0;
    mminfo.mainmem = WOLF3D_DOS_MAINMEM_BYTES;
    MM_GetPtr(&bufferseg, BUFFERSIZE);
}

void MM_Shutdown(void) {
    if (bufferseg) {
        free(bufferseg);
        bufferseg = nil;
    }
}

void MM_MapEMS(void) {
}

void MM_GetPtr(memptr* baseptr, unsigned long size) {
    void* ptr;

    if (!baseptr) {
        return;
    }
    if (!size) {
        *baseptr = nil;
        return;
    }
    ptr = malloc((size_t)size);
    if (!ptr) {
        mmerror = true;
        *baseptr = nil;
        fprintf(stderr, "MM_GetPtr: out of memory requesting %lu bytes\n", size);
        Quit("MM_GetPtr: out of memory");
        return;
    }
    memset(ptr, 0, (size_t)size);
    *baseptr = ptr;
}

void MM_FreePtr(memptr* baseptr) {
    if (baseptr && *baseptr) {
        free(*baseptr);
        *baseptr = nil;
    }
}

void MM_SetPurge(memptr* baseptr, int purge) {
    (void)baseptr;
    (void)purge;
}

void MM_SetLock(memptr* baseptr, boolean locked) {
    (void)baseptr;
    (void)locked;
}

void MM_SortMem(void) {
    if (beforesort) beforesort();
    if (aftersort) aftersort();
}

void MM_ShowMemory(void) {
}

long MM_UnusedMemory(void) {
    return WOLF3D_DOS_MAINMEM_BYTES;
}

long MM_TotalFree(void) {
    return MM_UnusedMemory();
}

void MM_BombOnError(boolean bomb) {
    (void)bomb;
}

void MML_UseSpace(unsigned segstart, unsigned seglength) {
    (void)segstart;
    (void)seglength;
}

void VL_Startup(void) {
    VL_SetLineWidth(SCREENWIDTH);
    (void)wolf3d_display_open();
}

static void wolf3d_display_release(void) {
    if (wolf3d_display_acquired) {
        (void)wolf3d_syscall0(WOLF3D_DISPLAY_RELEASE);
        wolf3d_display_acquired = 0;
    }
    wolf3d_display_cleared = 0;
    wolf3d_display_checked = 0;
    wolf3d_video_dirty = 0;
    wolf3d_video_deferred_present = 0;
    wolf3d_display_mapped = 0;
    memset(&wolf3d_display_map, 0, sizeof(wolf3d_display_map));
}

void VL_Shutdown(void) {
    wolf3d_display_release();
}

void VL_SetVGAPlane(void) {
}

void VL_SetTextMode(void) {
}

void VL_DePlaneVGA(void) {
}

void VL_SetVGAPlaneMode(void) {
    VL_SetLineWidth(SCREENWIDTH);
}

void VL_ClearVideo(byte color) {
    memset(wolf3d_video_planes, color, sizeof(wolf3d_video_planes));
    for (unsigned int page = 0; page < WOLF3D_VIDEO_PAGES; page++) {
        memset(wolf3d_video_pages[page], color, WOLF3D_SCREEN_PIXELS);
    }
    wolf3d_video_dirty = 1;
    wolf3d_video_present();
}

void VL_SetLineWidth(unsigned width) {
    unsigned y;

    linewidth = width;
    for (y = 0; y < MAXSCANLINES; ++y) {
        ylookup[y] = y * linewidth;
    }
}

void VL_SetSplitScreen(int linenum) {
    (void)linenum;
}

void VL_WaitVBL(int vbls) {
    if (vbls < 1) {
        vbls = 1;
    }
    wolf3d_wait_tics((unsigned int)vbls);
}

void VL_CrtcStart(int crtc) {
    (void)crtc;
}

void VL_SetScreen(int crtc, int pan) {
    displayofs = (unsigned)crtc;
    pelpan = (unsigned)pan;
    wolf3d_video_present();
}

void VL_FillPalette(int red, int green, int blue) {
    int i;
    byte red_channel = wolf3d_palette_channel(red);
    byte green_channel = wolf3d_palette_channel(green);
    byte blue_channel = wolf3d_palette_channel(blue);

    for (i = 0; i < 256; ++i) {
        wolf3d_palette[i * 3 + 0] = red_channel;
        wolf3d_palette[i * 3 + 1] = green_channel;
        wolf3d_palette[i * 3 + 2] = blue_channel;
    }
    wolf3d_video_present();
}

void VL_SetColor(int color, int red, int green, int blue) {
    if (color < 0 || color >= 256) {
        return;
    }
    wolf3d_palette[color * 3 + 0] = wolf3d_palette_channel(red);
    wolf3d_palette[color * 3 + 1] = wolf3d_palette_channel(green);
    wolf3d_palette[color * 3 + 2] = wolf3d_palette_channel(blue);
    wolf3d_video_present();
}

void VL_GetColor(int color, int* red, int* green, int* blue) {
    if (color < 0 || color >= 256) {
        if (red) *red = 0;
        if (green) *green = 0;
        if (blue) *blue = 0;
        return;
    }
    if (red) *red = wolf3d_palette[color * 3 + 0];
    if (green) *green = wolf3d_palette[color * 3 + 1];
    if (blue) *blue = wolf3d_palette[color * 3 + 2];
}

void VL_SetPalette(byte far* palette) {
    if (palette) {
        memcpy(wolf3d_palette, palette, sizeof(wolf3d_palette));
    }
    if (wolf3d_display_acquired || wolf3d_video_dirty) {
        wolf3d_video_present();
    }
}

void VL_GetPalette(byte far* palette) {
    if (palette) {
        memcpy(palette, wolf3d_palette, sizeof(wolf3d_palette));
    }
}

void VL_FadeOut(int start, int end, int red, int green, int blue, int steps) {
    byte original[768];
    byte faded[768];
    byte target_red = wolf3d_palette_channel(red);
    byte target_green = wolf3d_palette_channel(green);
    byte target_blue = wolf3d_palette_channel(blue);

    if (start < 0) start = 0;
    if (end > 255) end = 255;
    if (end < start) {
        return;
    }
    if (steps < 1) {
        steps = 1;
    }

    wolf3d_fade_wait_vbl();
    VL_GetPalette(original);
    memcpy(faded, original, sizeof(faded));
    for (int step = 0; step < steps; step++) {
        for (int color = start; color <= end; color++) {
            int base = color * 3;
            int orig_red = original[base + 0];
            int orig_green = original[base + 1];
            int orig_blue = original[base + 2];

            faded[base + 0] =
                (byte)(orig_red + ((int)target_red - orig_red) * step / steps);
            faded[base + 1] =
                (byte)(orig_green + ((int)target_green - orig_green) * step / steps);
            faded[base + 2] =
                (byte)(orig_blue + ((int)target_blue - orig_blue) * step / steps);
        }
        wolf3d_fade_wait_vbl();
        VL_SetPalette(faded);
    }
    VL_FillPalette(target_red, target_green, target_blue);
    screenfaded = true;
}

void VL_FadeIn(int start, int end, byte far* palette, int steps) {
    byte original[768];
    byte faded[768];

    if (!palette) {
        return;
    }
    if (start < 0) start = 0;
    if (end > 255) end = 255;
    if (end < start) {
        return;
    }
    if (steps < 1) {
        steps = 1;
    }

    wolf3d_fade_wait_vbl();
    VL_GetPalette(original);
    memcpy(faded, original, sizeof(faded));
    start *= 3;
    end = end * 3 + 2;
    for (int step = 0; step < steps; step++) {
        for (int index = start; index <= end; index++) {
            int orig = original[index];
            faded[index] =
                (byte)(orig + ((int)palette[index] - orig) * step / steps);
        }
        wolf3d_fade_wait_vbl();
        VL_SetPalette(faded);
    }
    VL_SetPalette(palette);
    screenfaded = false;
}

void VL_ColorBorder(int color) {
    bordercolor = (unsigned)color;
}

void VL_Plot(int x, int y, int color) {
    wolf3d_video_put(x, y, (byte)color);
}

void VL_Hlin(unsigned x, unsigned y, unsigned width, unsigned color) {
    for (unsigned int i = 0; i < width; i++) {
        wolf3d_video_put((int)(x + i), (int)y, (byte)color);
    }
    wolf3d_video_present_offset_if_visible(bufferofs);
}

void VL_Vlin(int x, int y, int height, int color) {
    for (int i = 0; i < height; i++) {
        wolf3d_video_put(x, y + i, (byte)color);
    }
    wolf3d_video_present_offset_if_visible(bufferofs);
}

void VL_Bar(int x, int y, int width, int height, int color) {
    for (int py = 0; py < height; py++) {
        for (int px = 0; px < width; px++) {
            wolf3d_video_put(x + px, y + py, (byte)color);
        }
    }
    wolf3d_video_present_offset_if_visible(bufferofs);
}

void VL_MungePic(byte far* source, unsigned width, unsigned height) {
    byte* temp;
    unsigned plane;
    unsigned x;
    unsigned y;
    unsigned pwidth;
    unsigned size;
    byte* dest;

    if (!source || !width || !height) {
        return;
    }
    size = width * height;
    temp = malloc(size);
    if (!temp) {
        Quit("VL_MungePic: out of memory");
        return;
    }
    memcpy(temp, source, size);
    dest = source;
    pwidth = width / 4;
    for (plane = 0; plane < 4; ++plane) {
        for (y = 0; y < height; ++y) {
            for (x = 0; x < pwidth; ++x) {
                *dest++ = temp[y * width + x * 4 + plane];
            }
        }
    }
    free(temp);
}

void VL_DrawPicBare(int x, int y, byte far* pic, int width, int height) {
    VL_MemToScreen(pic, width, height, x, y);
}

void VL_MemToLatch(byte far* source, int width, int height, unsigned dest) {
    unsigned int width_bytes;
    unsigned int plane_size;

    if (!source || width <= 0 || height <= 0) {
        return;
    }
    width_bytes = (unsigned int)((width + 3) >> 2);
    plane_size = width_bytes * (unsigned int)height;
    for (unsigned int plane = 0; plane < 4u; plane++) {
        for (unsigned int i = 0; i < plane_size; i++) {
            wolf3d_planar_store(dest + i, plane, source[plane * plane_size + i]);
        }
    }
}

void VL_ScreenToScreen(unsigned source, unsigned dest, int width, int height) {
    if (width <= 0 || height <= 0) {
        return;
    }
    for (int row = 0; row < height; row++) {
        unsigned int srcrow = source + (unsigned int)row * linewidth;
        unsigned int dstrow = dest + (unsigned int)row * linewidth;

        for (int col = 0; col < width; col++) {
            for (unsigned int plane = 0; plane < 4u; plane++) {
                wolf3d_planar_store(dstrow + (unsigned int)col, plane,
                                    wolf3d_planar_load(srcrow + (unsigned int)col, plane));
            }
        }
    }
    wolf3d_video_dirty = 1;
}

void VL_MemToScreen(byte far* source, int width, int height, int x, int y) {
    int width_bytes;
    int base_x;
    int mask;

    if (!source || width <= 0 || height <= 0) {
        return;
    }
    width_bytes = width >> 2;
    if (width_bytes <= 0) {
        return;
    }
    base_x = x & ~3;
    mask = 1 << (x & 3);
    for (int plane = 0; plane < 4; plane++) {
        int plane_index;

        if (mask == 1) {
            plane_index = 0;
        } else if (mask == 2) {
            plane_index = 1;
        } else if (mask == 4) {
            plane_index = 2;
        } else {
            plane_index = 3;
        }
        for (int row = 0; row < height; row++) {
            for (int col = 0; col < width_bytes; col++) {
                unsigned int offset;

                if (y + row < 0 || y + row >= (int)WOLF3D_SCREEN_H ||
                    base_x + col * 4 + plane_index < 0 ||
                    base_x + col * 4 + plane_index >= (int)WOLF3D_SCREEN_W) {
                    continue;
                }
                offset = bufferofs + ylookup[y + row] + (unsigned int)(base_x >> 2) +
                         (unsigned int)col;
                wolf3d_planar_store(offset, (unsigned int)plane_index,
                                    source[row * width_bytes + col]);
            }
        }
        source += width_bytes * height;
        mask <<= 1;
        if (mask == 16) {
            mask = 1;
        }
    }
    wolf3d_video_present_offset_if_visible(bufferofs);
}

void VL_MaskedToScreen(byte far* source, int width, int height, int x, int y) {
    VL_MemToScreen(source, width, height, x, y);
}

void VL_LatchToScreen(unsigned source, int width, int height, int x, int y) {
    if (width <= 0 || height <= 0) {
        return;
    }
    for (int row = 0; row < height; row++) {
        for (int col = 0; col < width; col++) {
            for (unsigned int plane = 0; plane < 4u; plane++) {
                int dx = x + col * 4 + (int)plane;
                int dy = y + row;
                byte color;

                if (dx < 0 || dy < 0 || dx >= (int)WOLF3D_SCREEN_W ||
                    dy >= (int)WOLF3D_SCREEN_H) {
                    continue;
                }
                color = wolf3d_planar_load(source + (unsigned int)row * (unsigned int)width +
                                           (unsigned int)col, plane);
                wolf3d_video_put(dx, dy, color);
            }
        }
    }
    wolf3d_video_present_offset_if_visible(bufferofs);
}

static fontstruct* wolf3d_font(int masked) {
    int chunk = (masked && NUMFONTM > 0) ? STARTFONTM + fontnumber
                                         : STARTFONT + fontnumber;

    if (chunk < 0 || chunk >= NUMCHUNKS || !grsegs[chunk]) {
        return NULL;
    }
    return (fontstruct*)grsegs[chunk];
}

static void wolf3d_measure_font_string(char far* string, word* width,
                                       word* height, fontstruct* font) {
    word w = 0;

    if (!font) {
        if (width) *width = string ? (word)(strlen(string) * 8u) : 0;
        if (height) *height = 8;
        return;
    }
    if (string) {
        while (*string) {
            w = (word)(w + (unsigned char)font->width[(byte)*string++]);
        }
    }
    if (width) *width = w;
    if (height) *height = (word)font->height;
}

static void wolf3d_draw_font_string(char far* string, int masked) {
    fontstruct* font = wolf3d_font(masked);
    int startx = px;

    if (!string) {
        bufferwidth = 0;
        bufferheight = 0;
        return;
    }
    if (!font) {
        px += (int)strlen(string) * 8;
        bufferwidth = px - startx;
        bufferheight = 8;
        return;
    }
    bufferheight = font->height;
    while (*string) {
        byte ch = (byte)*string++;
        int width = (unsigned char)font->width[ch];
        byte* source = ((byte*)font) + font->location[ch];

        for (int col = 0; col < width; col++) {
            for (int row = 0; row < font->height; row++) {
                if (source[col + row * width]) {
                    wolf3d_video_put(px + col, py + row, fontcolor);
                }
            }
        }
        px += width;
    }
    bufferwidth = px - startx;
}

void VL_DrawTile8String(char* str, char far* tile8ptr, int printx, int printy) {
    if (!str || !tile8ptr) {
        return;
    }
    while (*str) {
        unsigned int tile = (unsigned char)*str++;

        VL_MemToScreen((byte far*)tile8ptr + tile * 64u, 8, 8, printx, printy);
        printx += 8;
    }
}

void VL_DrawLatch8String(char* str, unsigned tile8ptr, int printx, int printy) {
    if (!str || !tile8ptr) {
        return;
    }
    while (*str) {
        unsigned int tile = (unsigned char)*str++;

        VL_LatchToScreen(tile8ptr + tile * 16u, 2, 8, printx, printy);
        printx += 8;
    }
}

void VL_SizeTile8String(char* str, int* width, int* height) {
    if (width) *width = str ? (int)strlen(str) * 8 : 0;
    if (height) *height = 8;
}

void VL_DrawPropString(char* str, unsigned tile8ptr, int printx, int printy) {
    (void)tile8ptr;
    px = printx;
    py = printy;
    wolf3d_draw_font_string(str, false);
}

void VL_SizePropString(char* str, int* width, int* height, char far* font) {
    word w;
    word h;

    wolf3d_measure_font_string(str, &w, &h, (fontstruct*)font);
    if (width) *width = w;
    if (height) *height = h;
}

void VL_TestPaletteSet(void) {
}

void VW_InitDoubleBuffer(void) {
}

int VW_MarkUpdateBlock(int x1, int y1, int x2, int y2) {
    (void)x1;
    (void)y1;
    (void)x2;
    (void)y2;
    return 1;
}

void VW_UpdateScreen(void) {
    wolf3d_video_commit_buffer();
}

void VH_UpdateScreen(void) {
    wolf3d_video_commit_buffer();
}

void VH_DrawPic(int x, int y, int chunknum) {
    VWB_DrawPic(x, y, chunknum);
}

void VWB_DrawTile8(int x, int y, int tile) {
    if (NUMTILE8 > 0 && latchpics[0]) {
        VL_LatchToScreen(latchpics[0] + (unsigned)tile * 16u, 2, 8, x, y);
    }
}

void VWB_DrawTile8M(int x, int y, int tile) {
    if (NUMTILE8M > 0 && STARTTILE8M < NUMCHUNKS && grsegs[STARTTILE8M]) {
        VL_MemToScreen((byte far*)grsegs[STARTTILE8M] + (unsigned)tile * 64u,
                       8, 8, x, y);
    } else {
        VWB_DrawTile8(x, y, tile);
    }
}

void VWB_DrawTile16(int x, int y, int tile) {
    if (NUMTILE16 > 0 && latchpics[1]) {
        VL_LatchToScreen(latchpics[1] + (unsigned)tile * 64u, 4, 16, x, y);
    }
}

void VWB_DrawTile16M(int x, int y, int tile) {
    if (NUMTILE16M > 0 && STARTTILE16M + tile < NUMCHUNKS &&
        grsegs[STARTTILE16M + tile]) {
        VL_MemToScreen((byte far*)grsegs[STARTTILE16M + tile], 16, 16, x, y);
    } else {
        VWB_DrawTile16(x, y, tile);
    }
}

void VWB_DrawPic(int x, int y, int chunknum) {
    pictabletype* pic;

    x &= ~7;
    if (chunknum < STARTPICS || chunknum >= STARTPICS + NUMPICS ||
        chunknum >= NUMCHUNKS || !grsegs[chunknum] || !pictable) {
        return;
    }
    pic = &pictable[chunknum - STARTPICS];
    if (pic->width <= 0 || pic->height <= 0) {
        return;
    }
    VL_MemToScreen((byte far*)grsegs[chunknum], pic->width, pic->height, x, y);
}

void VWB_DrawMPic(int x, int y, int chunknum) {
    VWB_DrawPic(x, y, chunknum);
}

void VWB_Bar(int x, int y, int width, int height, int color) {
    VL_Bar(x, y, width, height, color);
}

void VWB_DrawPropString(char far* string) {
    int x = px;

    wolf3d_draw_font_string(string, false);
    (void)VW_MarkUpdateBlock(x, py, px - 1, py + bufferheight - 1);
    wolf3d_video_present_offset_if_visible(bufferofs);
}

void VWB_DrawMPropString(char far* string) {
    int x = px;

    wolf3d_draw_font_string(string, true);
    (void)VW_MarkUpdateBlock(x, py, px - 1, py + bufferheight - 1);
    wolf3d_video_present_offset_if_visible(bufferofs);
}

void VWB_DrawSprite(int x, int y, int chunknum) {
    (void)x;
    (void)y;
    (void)chunknum;
}

void VWB_Plot(int x, int y, int color) {
    VL_Plot(x, y, color);
}

void VWB_Hlin(int x1, int x2, int y, int color) {
    if (x2 >= x1) {
        VL_Hlin((unsigned)x1, (unsigned)y, (unsigned)(x2 - x1 + 1), (unsigned)color);
    }
}

void VWB_Vlin(int y1, int y2, int x, int color) {
    if (y2 >= y1) {
        VL_Vlin(x, y1, y2 - y1 + 1, color);
    }
}

void VH_SetDefaultColors(void) {
    fontcolor = WHITE;
    backcolor = BLACK;
}

void VW_MeasurePropString(char far* string, word* width, word* height) {
    wolf3d_measure_font_string(string, width, height, wolf3d_font(false));
}

void wolf3d_visual_smoke_frame(void) {
    if (grstarts) {
        CA_CacheScreen(TITLEPIC);
        VW_UpdateScreen();
    } else {
        wolf3d_video_present_page(wolf3d_page_from_offset(displayofs));
    }
    VL_WaitVBL(84);
}

void LatchDrawPic(unsigned x, unsigned y, unsigned picnum) {
    pictabletype* pic;
    unsigned int index;

    if (!pictable || picnum < STARTPICS ||
        picnum < LATCHPICS_LUMP_START || picnum > LATCHPICS_LUMP_END) {
        return;
    }
    index = 2u + picnum - LATCHPICS_LUMP_START;
    if (index >= NUMLATCHPICS || !latchpics[index]) {
        return;
    }
    pic = &pictable[picnum - STARTPICS];
    if (pic->width <= 0 || pic->height <= 0) {
        return;
    }
    VL_LatchToScreen(latchpics[index], pic->width / 4, pic->height,
                     (int)x * 8, (int)y);
}

void LoadLatchMem(void) {
    int start;
    int end;
    unsigned destoff = freelatch;

    memset(latchpics, 0, sizeof(unsigned) * NUMLATCHPICS);
    if (NUMTILE8 > 0 && STARTTILE8 < NUMCHUNKS) {
        CA_CacheGrChunk(STARTTILE8);
        if (grsegs[STARTTILE8]) {
            latchpics[0] = destoff;
            VL_MemToLatch((byte far*)grsegs[STARTTILE8], 8, 8 * NUMTILE8, destoff);
            destoff += (unsigned)NUMTILE8 * 16u;
            UNCACHEGRCHUNK(STARTTILE8);
        }
    }
    start = LATCHPICS_LUMP_START;
    end = LATCHPICS_LUMP_END;
    for (int i = start; i <= end; i++) {
        pictabletype* pic;
        unsigned int index = 2u + (unsigned)i - (unsigned)start;

        if (index >= NUMLATCHPICS || i < STARTPICS || i >= NUMCHUNKS || !pictable) {
            continue;
        }
        pic = &pictable[i - STARTPICS];
        if (pic->width <= 0 || pic->height <= 0) {
            continue;
        }
        CA_CacheGrChunk(i);
        if (grsegs[i]) {
            latchpics[index] = destoff;
            VL_MemToLatch((byte far*)grsegs[i], pic->width, pic->height, destoff);
            destoff += (unsigned)(pic->width / 4) * (unsigned)pic->height;
            UNCACHEGRCHUNK(i);
        }
    }
}

boolean FizzleFade(unsigned source, unsigned dest, unsigned width,
                   unsigned height, unsigned frames, boolean abortable) {
    (void)source;
    (void)dest;
    (void)width;
    (void)height;
    (void)frames;
    (void)abortable;
    return false;
}

void alOut(byte n, byte b) {
    (void)wolf3d_adlib_out(n, b);
}

static void wolf3d_setup_digi(void) {
    word* source;
    word page;
    int count;

    for (int i = 0; i < LASTSOUND; i++) {
        DigiMap[i] = -1;
    }

    if (DigiList || ChunksInFile == 0 || PMSoundStart == 0) {
        return;
    }

    source = (word*)PM_GetPage(ChunksInFile - 1);
    page = PMSoundStart;
    for (count = 0; count < PMPageSize / (int)(sizeof(word) * 2u);
         count++) {
        unsigned int bytes;

        if (page >= ChunksInFile - 1) {
            break;
        }
        bytes = source[count * 2 + 1];
        page = (word)(page + ((bytes + PMPageSize - 1u) / PMPageSize));
    }

    if (count <= 0) {
        return;
    }

    DigiList = (word*)malloc((size_t)count * sizeof(word) * 2u);
    if (!DigiList) {
        return;
    }
    memcpy(DigiList, source, (size_t)count * sizeof(word) * 2u);
    NumDigi = (word)count;
}

static int wolf3d_play_pcm_u8(const unsigned char* samples,
                              unsigned int count,
                              unsigned int sample_hz) {
    sys_sound_pcm_u8_t req;

    req.samples = samples;
    req.count = count;
    req.sample_hz = sample_hz;
    return wolf3d_sound_op(SYS_SOUND_OP_PCM_U8, (unsigned int)&req, 0u);
}

static int wolf3d_play_digi_sample(word page, unsigned int length) {
    longword ticks;
    uint32_t duration_units;
    unsigned int copied;
    unsigned int page_offset = 0u;

    if (length == 0u || length > SYS_SOUND_PCM_MAX_SAMPLES) {
        return 0;
    }
    if (wolf3d_digi_buffer_capacity < length) {
        byte* buffer = (byte*)realloc(wolf3d_digi_buffer, length);

        if (!buffer) {
            return 0;
        }
        wolf3d_digi_buffer = buffer;
        wolf3d_digi_buffer_capacity = length;
    }

    copied = 0u;
    while (copied < length) {
        unsigned int page_left = PMPageSize - page_offset;
        unsigned int part = length - copied;
        byte* source;

        if (part > page_left) {
            part = page_left;
        }
        source = (byte*)PM_GetSoundPage(page);
        memcpy(wolf3d_digi_buffer + copied, source + page_offset, part);

        copied += part;
        page_offset += part;
        if (page_offset >= PMPageSize) {
            page++;
            page_offset = 0u;
        }
    }

    ticks = (longword)((length + WOLF3D_DIGI_BYTES_PER_TIC - 1u) /
                       WOLF3D_DIGI_BYTES_PER_TIC);
    if (ticks == 0) {
        ticks = 1;
    }

    duration_units = (length * WOLF3D_TIMER_UNITS_PER_SECOND +
                      WOLF3D_DIGI_SAMPLE_HZ - 1u) /
                     WOLF3D_DIGI_SAMPLE_HZ;
    if (!duration_units) {
        duration_units = 1u;
    }

    if (wolf3d_play_pcm_u8(wolf3d_digi_buffer, length,
                           WOLF3D_DIGI_SAMPLE_HZ) < 0) {
        return 0;
    }

    wolf3d_digi_end_tick = TimeCount + ticks;
    wolf3d_digi_end_units = wolf3d_now_units() + duration_units;
    DigiPlaying = true;
    return 1;
}

void SD_Startup(void) {
    int caps = wolf3d_sound_op(SYS_SOUND_OP_CAPS, 0u, 0u);

    AdLibPresent = (caps & SYS_SOUND_CAP_ADLIB) != 0;
    SoundSourcePresent = false;
#if WOLF3D_ENABLE_SB_DIGI
    SoundBlasterPresent = (caps & SYS_SOUND_CAP_PCM_U8) != 0;
#else
    SoundBlasterPresent = false;
#endif
    NeedsMusic = false;
    SoundPositioned = false;
    SoundMode = sdm_Off;
    DigiMode = sds_Off;
    MusicMode = smm_Off;
    DigiPlaying = false;
    wolf3d_setup_digi();
    if (AdLibPresent) {
        (void)wolf3d_sound_op(SYS_SOUND_OP_OPL_RESET, 0u, 0u);
    }
    wolf3d_sound_stop();
}

void SD_Shutdown(void) {
    wolf3d_sound_stop();
}

void SD_Default(boolean gotit, SDMode sd, SMMode sm) {
    (void)gotit;
    (void)SD_SetSoundMode(sd);
    (void)SD_SetMusicMode(sm);
}

void SD_PositionSound(int leftvol, int rightvol) {
    (void)leftvol;
    (void)rightvol;
}

boolean SD_PlaySound(soundnames sound) {
    void* sound_chunk;
    const byte* pc_data;
    sys_sound_pit_sequence_t seq;
    int chunk;
    longword length;
    word priority;
    unsigned int count;

    wolf3d_sync_time_count();

    if ((int)sound < 0 || sound >= LASTSOUND) {
        return false;
    }

    chunk = (SoundMode == sdm_AdLib && AdLibPresent)
        ? STARTADLIBSOUNDS + (int)sound
        : STARTPCSOUNDS + (int)sound;
    if (!audiosegs[chunk]) {
        CA_CacheAudioChunk(chunk);
    }
    if (!audiosegs[chunk]) {
        return false;
    }

    sound_chunk = audiosegs[chunk];
    length = wolf3d_sound_chunk_length(sound_chunk);
    priority = wolf3d_sound_chunk_priority(sound_chunk);
    if (!length) {
        return false;
    }

    if (DigiMode == sds_SoundBlaster && SoundBlasterPresent &&
        DigiMap[sound] >= 0) {
        if (wolf3d_digi_is_playing() && priority < wolf3d_digi_priority) {
            return false;
        }
        SD_PlayDigitized((word)DigiMap[sound], 0, 0);
        if (DigiPlaying) {
            wolf3d_digi_number = (word)sound;
            wolf3d_digi_priority = priority;
            SoundPositioned = false;
            return true;
        }
    }

    if (SoundMode == sdm_Off) {
        return false;
    }

    if (SoundMode == sdm_AdLib) {
        return wolf3d_adlib_play_sound(sound, sound_chunk) ? true : false;
    }

    if (wolf3d_pc_sound_is_playing() && priority < wolf3d_pc_sound_priority) {
        return false;
    }
    count = length;
    if (count > SYS_SOUND_SEQUENCE_MAX_SAMPLES) {
        count = SYS_SOUND_SEQUENCE_MAX_SAMPLES;
    }

    pc_data = wolf3d_pc_sound_chunk_data(sound_chunk);
    seq.samples = pc_data;
    seq.count = count;
    seq.sample_hz = WOLF3D_TICS_PER_SECOND * 2u;
    seq.divisor_scale = 60u;
    if (wolf3d_sound_op(SYS_SOUND_OP_PIT_SEQUENCE,
                        (unsigned int)&seq, 0u) < 0) {
        return false;
    }

    wolf3d_pc_sound_data = (byte*)pc_data;
    wolf3d_pc_sound_length = count;
    wolf3d_pc_sound_end_tick = TimeCount + (longword)((count + 1u) / 2u);
    wolf3d_pc_sound_number = (word)sound;
    wolf3d_pc_sound_priority = priority;
    return false;
}

void SD_SetPosition(int leftvol, int rightvol) {
    SD_PositionSound(leftvol, rightvol);
}

void SD_StopSound(void) {
    wolf3d_stop_sound_effects();
    SoundPositioned = false;
}

void SD_WaitSoundDone(void) {
    while (SD_SoundPlaying()) {
        wolf3d_sync_time_count();
        (void)usleep(1000u);
    }
}

void SD_StartMusic(MusicGroup far* music) {
    wolf3d_adlib_music_clear();
    if (MusicMode != smm_AdLib || !AdLibPresent || !music ||
        music->length < 4u) {
        return;
    }

    wolf3d_music_data = music->values;
    wolf3d_music_ptr = music->values;
    wolf3d_music_len_bytes = music->length;
    wolf3d_music_left_bytes = music->length;
    wolf3d_music_next_units = wolf3d_now_units();
    wolf3d_music_active = 1;
    NeedsMusic = true;
}

void SD_MusicOn(void) {
    if (wolf3d_music_data && MusicMode == smm_AdLib && AdLibPresent) {
        wolf3d_music_next_units = wolf3d_now_units();
        wolf3d_music_active = 1;
    }
}

void SD_MusicOff(void) {
    wolf3d_adlib_music_silence();
}

void SD_FadeOutMusic(void) {
    SD_MusicOff();
}

void SD_SetUserHook(void (*hook)(void)) {
    (void)hook;
}

boolean SD_MusicPlaying(void) {
    return wolf3d_music_active ? true : false;
}

boolean SD_SetSoundMode(SDMode mode) {
    if (mode == sdm_AdLib && !AdLibPresent) {
        mode = sdm_PC;
    }
    if (mode < sdm_Off || mode > sdm_AdLib) {
        return false;
    }
    if (SoundMode != mode) {
        wolf3d_stop_sound_effects();
    }
    SoundMode = mode;
    return true;
}

boolean SD_SetMusicMode(SMMode mode) {
    if (mode == smm_AdLib && !AdLibPresent) {
        mode = smm_Off;
    }
    if (mode < smm_Off || mode > smm_AdLib) {
        return false;
    }
    if (mode == smm_Off && MusicMode != smm_Off) {
        SD_MusicOff();
    }
    MusicMode = mode;
    NeedsMusic = mode != smm_Off;
    return true;
}

word SD_SoundPlaying(void) {
    wolf3d_sync_time_count();
    if (wolf3d_pc_sound_is_playing()) {
        return wolf3d_pc_sound_number;
    }
    if (wolf3d_al_sound_data) {
        return wolf3d_al_sound_number;
    }
    /*
     * Match the original Wolf3D sound manager: digitized SB playback is
     * tracked separately with DigiPlaying and does not make SD_SoundPlaying()
     * block callers such as SD_WaitSoundDone().
     */
    return 0;
}

void SD_SetDigiDevice(SDSMode mode) {
    if (mode == sds_SoundBlaster && !SoundBlasterPresent) {
        mode = sds_Off;
    }
    if (mode != sds_Off && mode != sds_SoundBlaster) {
        mode = sds_Off;
    }
    if (DigiMode != mode) {
        SD_StopDigitized();
    }
    DigiMode = mode;
}

void SD_PlayDigitized(word which, int leftpos, int rightpos) {
    (void)leftpos;
    (void)rightpos;
    unsigned int remaining;

    wolf3d_sync_time_count();
    if (DigiMode != sds_SoundBlaster || !SoundBlasterPresent) {
        return;
    }
    if (!DigiList || which >= NumDigi) {
        return;
    }

    remaining = DigiList[which * 2u + 1u];
    if (remaining == 0u) {
        return;
    }

    DigiPlaying = false;
    wolf3d_digi_end_tick = TimeCount;
    wolf3d_digi_end_units = wolf3d_now_units();
    wolf3d_digi_number = which;
    if (!wolf3d_play_digi_sample(DigiList[which * 2u], remaining)) {
        wolf3d_digi_end_tick = 0;
        wolf3d_digi_end_units = 0;
        DigiPlaying = false;
        return;
    }
}

void SD_StopDigitized(void) {
    DigiPlaying = false;
    wolf3d_digi_end_tick = 0;
    wolf3d_digi_end_units = 0;
    wolf3d_digi_priority = 0;
    wolf3d_digi_number = 0;
    (void)wolf3d_sound_op(SYS_SOUND_OP_STOP, 0u, 0u);
}

void SD_Poll(void) {
    wolf3d_sync_time_count();
}

void PM_SetMainPurge(int level) {
    PM_SetMainMemPurge(level);
}
