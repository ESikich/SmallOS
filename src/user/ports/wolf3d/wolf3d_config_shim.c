#include "wolf3d_port.h"

#include <unistd.h>

#include "WL_DEF.H"

static int wolf3d_read_exact(int file, void* buffer, unsigned int size) {
    unsigned int done = 0;
    byte* out = (byte*)buffer;

    while (done < size) {
        int got = read(file, out + done, size - done);
        if (got <= 0) {
            return 0;
        }
        done += (unsigned int)got;
    }
    return 1;
}

static int wolf3d_write_exact(int file, const void* buffer, unsigned int size) {
    unsigned int done = 0;
    const byte* in = (const byte*)buffer;

    while (done < size) {
        int sent = write(file, in + done, size - done);
        if (sent <= 0) {
            return 0;
        }
        done += (unsigned int)sent;
    }
    return 1;
}

static int wolf3d_read_config_i16(int file, int* value) {
    byte bytes[2];
    uint16_t raw;

    if (!wolf3d_read_exact(file, bytes, sizeof(bytes))) {
        return 0;
    }
    raw = (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
    *value = (int)(int16_t)raw;
    return 1;
}

static int wolf3d_read_config_u16(int file, word* value) {
    int raw;

    if (!wolf3d_read_config_i16(file, &raw)) {
        return 0;
    }
    *value = (word)(uint16_t)raw;
    return 1;
}

static int wolf3d_read_config_i32(int file, long* value) {
    byte bytes[4];
    uint32_t raw;

    if (!wolf3d_read_exact(file, bytes, sizeof(bytes))) {
        return 0;
    }
    raw = (uint32_t)bytes[0] |
          ((uint32_t)bytes[1] << 8) |
          ((uint32_t)bytes[2] << 16) |
          ((uint32_t)bytes[3] << 24);
    *value = (long)(int32_t)raw;
    return 1;
}

static int wolf3d_write_config_i16(int file, int value) {
    uint16_t raw = (uint16_t)value;
    byte bytes[2];

    bytes[0] = (byte)(raw & 0xffu);
    bytes[1] = (byte)(raw >> 8);
    return wolf3d_write_exact(file, bytes, sizeof(bytes));
}

static int wolf3d_write_config_i32(int file, long value) {
    uint32_t raw = (uint32_t)value;
    byte bytes[4];

    bytes[0] = (byte)(raw & 0xffu);
    bytes[1] = (byte)((raw >> 8) & 0xffu);
    bytes[2] = (byte)((raw >> 16) & 0xffu);
    bytes[3] = (byte)(raw >> 24);
    return wolf3d_write_exact(file, bytes, sizeof(bytes));
}

static unsigned int wolf3d_config_dos_size(unsigned int max_scores) {
    return max_scores * ((MaxHighName + 1u) + 4u + 2u + 2u) +
           3u * 2u +
           4u * 2u +
           1u * 2u +
           (4u + NUMBUTTONS + 4u + 4u) * 2u +
           2u * 2u;
}

static unsigned int wolf3d_config_legacy_size(unsigned int max_scores) {
    return sizeof(HighScore) * max_scores +
           sizeof(SoundMode) + sizeof(MusicMode) + sizeof(DigiMode) +
           sizeof(mouseenabled) + sizeof(joystickenabled) +
           sizeof(joypadenabled) + sizeof(joystickprogressive) +
           sizeof(joystickport) +
           sizeof(dirscan) + sizeof(buttonscan) +
           sizeof(buttonmouse) + sizeof(buttonjoy) +
           sizeof(viewsize) + sizeof(mouseadjustment);
}

static void wolf3d_config_default_sound_modes(SDMode* sd, SMMode* sm,
                                              SDSMode* sds) {
    if (SoundBlasterPresent || AdLibPresent) {
        *sd = sdm_AdLib;
        *sm = smm_AdLib;
    } else {
        *sd = sdm_PC;
        *sm = smm_Off;
    }

    if (SoundBlasterPresent) {
        *sds = sds_SoundBlaster;
    } else if (SoundSourcePresent) {
        *sds = sds_SoundSource;
    } else {
        *sds = sds_Off;
    }
}

static void wolf3d_config_defaults(SDMode* sd, SMMode* sm, SDSMode* sds) {
    wolf3d_config_default_sound_modes(sd, sm, sds);

    mouseenabled = MousePresent ? true : false;
    joystickenabled = false;
    joypadenabled = false;
    joystickport = 0;
    joystickprogressive = false;
    viewsize = 15;
    mouseadjustment = 5;
    wolf3d_reset_control_bindings();
}

static int wolf3d_config_sound_modes_valid(SDMode sd, SMMode sm,
                                           SDSMode sds) {
    return sd >= sdm_Off && sd <= sdm_AdLib &&
           sm >= smm_Off && sm <= smm_AdLib &&
           sds >= sds_Off && sds <= sds_SoundBlaster;
}

static int wolf3d_read_config_high_scores(int file, HighScore* scores,
                                          unsigned int max_scores) {
    for (unsigned int i = 0; i < max_scores; i++) {
        if (!wolf3d_read_exact(file, scores[i].name, MaxHighName + 1u)) {
            return 0;
        }
        scores[i].name[MaxHighName] = '\0';
        if (!wolf3d_read_config_i32(file, &scores[i].score)) {
            return 0;
        }
        if (!wolf3d_read_config_u16(file, &scores[i].completed)) {
            return 0;
        }
        if (!wolf3d_read_config_u16(file, &scores[i].episode)) {
            return 0;
        }
    }
    return 1;
}

static int wolf3d_write_config_high_scores(int file, const HighScore* scores,
                                           unsigned int max_scores) {
    for (unsigned int i = 0; i < max_scores; i++) {
        if (!wolf3d_write_exact(file, scores[i].name, MaxHighName + 1u)) {
            return 0;
        }
        if (!wolf3d_write_config_i32(file, scores[i].score)) {
            return 0;
        }
        if (!wolf3d_write_config_i16(file, scores[i].completed)) {
            return 0;
        }
        if (!wolf3d_write_config_i16(file, scores[i].episode)) {
            return 0;
        }
    }
    return 1;
}

static int wolf3d_read_config_dos(int file, HighScore* scores,
                                  unsigned int max_scores, SDMode* sd,
                                  SMMode* sm, SDSMode* sds) {
    int value;

    if (!wolf3d_read_config_high_scores(file, scores, max_scores)) {
        return 0;
    }

    if (!wolf3d_read_config_i16(file, &value)) return 0;
    *sd = (SDMode)value;
    if (!wolf3d_read_config_i16(file, &value)) return 0;
    *sm = (SMMode)value;
    if (!wolf3d_read_config_i16(file, &value)) return 0;
    *sds = (SDSMode)value;

    if (!wolf3d_read_config_i16(file, &value)) return 0;
    mouseenabled = value ? true : false;
    if (!wolf3d_read_config_i16(file, &value)) return 0;
    joystickenabled = value ? true : false;
    if (!wolf3d_read_config_i16(file, &value)) return 0;
    joypadenabled = value ? true : false;
    if (!wolf3d_read_config_i16(file, &value)) return 0;
    joystickprogressive = value ? true : false;
    if (!wolf3d_read_config_i16(file, &joystickport)) return 0;

    for (unsigned int i = 0; i < 4u; i++) {
        if (!wolf3d_read_config_i16(file, &dirscan[i])) return 0;
    }
    for (unsigned int i = 0; i < NUMBUTTONS; i++) {
        if (!wolf3d_read_config_i16(file, &buttonscan[i])) return 0;
    }
    for (unsigned int i = 0; i < 4u; i++) {
        if (!wolf3d_read_config_i16(file, &buttonmouse[i])) return 0;
    }
    for (unsigned int i = 0; i < 4u; i++) {
        if (!wolf3d_read_config_i16(file, &buttonjoy[i])) return 0;
    }

    if (!wolf3d_read_config_i16(file, &viewsize)) return 0;
    if (!wolf3d_read_config_i16(file, &mouseadjustment)) return 0;
    return 1;
}

static int wolf3d_read_config_legacy(int file, HighScore* scores,
                                     unsigned int max_scores, SDMode* sd,
                                     SMMode* sm, SDSMode* sds) {
    return wolf3d_read_exact(file, scores, sizeof(HighScore) * max_scores) &&
           wolf3d_read_exact(file, sd, sizeof(*sd)) &&
           wolf3d_read_exact(file, sm, sizeof(*sm)) &&
           wolf3d_read_exact(file, sds, sizeof(*sds)) &&
           wolf3d_read_exact(file, &mouseenabled, sizeof(mouseenabled)) &&
           wolf3d_read_exact(file, &joystickenabled,
                             sizeof(joystickenabled)) &&
           wolf3d_read_exact(file, &joypadenabled, sizeof(joypadenabled)) &&
           wolf3d_read_exact(file, &joystickprogressive,
                             sizeof(joystickprogressive)) &&
           wolf3d_read_exact(file, &joystickport, sizeof(joystickport)) &&
           wolf3d_read_exact(file, dirscan, sizeof(dirscan)) &&
           wolf3d_read_exact(file, buttonscan, sizeof(buttonscan)) &&
           wolf3d_read_exact(file, buttonmouse, sizeof(buttonmouse)) &&
           wolf3d_read_exact(file, buttonjoy, sizeof(buttonjoy)) &&
           wolf3d_read_exact(file, &viewsize, sizeof(viewsize)) &&
           wolf3d_read_exact(file, &mouseadjustment,
                             sizeof(mouseadjustment));
}

void wolf3d_read_config_file(int file, void* scores, unsigned int max_scores,
                             void* sd, void* sm, void* sds) {
    HighScore* high_scores = (HighScore*)scores;
    SDMode* sound_mode = (SDMode*)sd;
    SMMode* music_mode = (SMMode*)sm;
    SDSMode* digi_mode = (SDSMode*)sds;
    int start = lseek(file, 0, SEEK_CUR);
    int end = lseek(file, 0, SEEK_END);
    unsigned int dos_size = wolf3d_config_dos_size(max_scores);
    unsigned int legacy_size = wolf3d_config_legacy_size(max_scores);
    int remaining = -1;
    int ok;

    if (start >= 0) {
        (void)lseek(file, start, SEEK_SET);
    }
    if (start >= 0 && end >= start) {
        remaining = end - start;
    }

    if (remaining == (int)legacy_size && legacy_size != dos_size) {
        ok = wolf3d_read_config_legacy(file, high_scores, max_scores,
                                       sound_mode, music_mode, digi_mode);
    } else if (remaining == (int)dos_size || remaining < 0) {
        ok = wolf3d_read_config_dos(file, high_scores, max_scores, sound_mode,
                                    music_mode, digi_mode);
    } else {
        ok = 0;
    }

    if (!ok) {
        wolf3d_config_defaults(sound_mode, music_mode, digi_mode);
    } else if (!wolf3d_config_sound_modes_valid(*sound_mode, *music_mode,
                                                *digi_mode)) {
        wolf3d_config_default_sound_modes(sound_mode, music_mode, digi_mode);
    }
}

void wolf3d_write_config_file(int file, const void* scores,
                              unsigned int max_scores, int sd, int sm,
                              int sds) {
    const HighScore* high_scores = (const HighScore*)scores;

    (void)wolf3d_write_config_high_scores(file, high_scores, max_scores);
    (void)wolf3d_write_config_i16(file, sd);
    (void)wolf3d_write_config_i16(file, sm);
    (void)wolf3d_write_config_i16(file, sds);

    (void)wolf3d_write_config_i16(file, mouseenabled);
    (void)wolf3d_write_config_i16(file, joystickenabled);
    (void)wolf3d_write_config_i16(file, joypadenabled);
    (void)wolf3d_write_config_i16(file, joystickprogressive);
    (void)wolf3d_write_config_i16(file, joystickport);

    for (unsigned int i = 0; i < 4u; i++) {
        (void)wolf3d_write_config_i16(file, dirscan[i]);
    }
    for (unsigned int i = 0; i < NUMBUTTONS; i++) {
        (void)wolf3d_write_config_i16(file, buttonscan[i]);
    }
    for (unsigned int i = 0; i < 4u; i++) {
        (void)wolf3d_write_config_i16(file, buttonmouse[i]);
    }
    for (unsigned int i = 0; i < 4u; i++) {
        (void)wolf3d_write_config_i16(file, buttonjoy[i]);
    }

    (void)wolf3d_write_config_i16(file, viewsize);
    (void)wolf3d_write_config_i16(file, mouseadjustment);
}
