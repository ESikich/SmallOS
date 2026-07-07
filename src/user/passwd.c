#include "crypt.h"
#include "errno.h"
#include "fcntl.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "sys/stat.h"
#include "sys/types.h"
#include "termios.h"
#include "time.h"
#include "unistd.h"

#define PASSWD_LINE_MAX 256
#define PASSWD_FIELD_MAX 96
#define PASSWD_HASH_MAX 128
#define PASSWD_SALT_LEN 16

static void strip_newline(char* s) {
    if (!s) return;
    for (unsigned int i = 0; s[i]; i++) {
        if (s[i] == '\n' || s[i] == '\r') {
            s[i] = '\0';
            return;
        }
    }
}

static void copy_field(char* dst, unsigned int dst_size, const char* src) {
    unsigned int i = 0;
    if (!dst || dst_size == 0) return;
    if (src) {
        while (src[i] && i + 1u < dst_size) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

static int passwd_user_exists(const char* name) {
    FILE* fp;
    char line[PASSWD_LINE_MAX];

    if (!name || !name[0]) return 0;
    fp = fopen("/etc/passwd", "r");
    if (!fp) return 0;
    while (fgets(line, sizeof(line), fp)) {
        char* colon;
        strip_newline(line);
        colon = strchr(line, ':');
        if (colon) *colon = '\0';
        if (strcmp(line, name) == 0) {
            fclose(fp);
            return 1;
        }
    }
    fclose(fp);
    return 0;
}

static int current_username(char* out, unsigned int out_size) {
    FILE* fp;
    char line[PASSWD_LINE_MAX];
    uid_t uid = getuid();

    if (!out || out_size == 0) return 0;
    fp = fopen("/etc/passwd", "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            char* fields[7];
            char* p = line;
            int valid = 1;
            strip_newline(line);
            for (unsigned int i = 0; i < 7; i++) {
                fields[i] = p;
                if (i == 6) break;
                while (*p && *p != ':') p++;
                if (*p != ':') {
                    valid = 0;
                    break;
                }
                *p++ = '\0';
            }
            if (valid && (uid_t)strtoul(fields[2], 0, 10) == uid) {
                copy_field(out, out_size, fields[0]);
                fclose(fp);
                return out[0] != '\0';
            }
        }
        fclose(fp);
    }

    if (uid == 0) {
        copy_field(out, out_size, "root");
        return 1;
    }
    return 0;
}

static int read_password(const char* prompt, char* out, unsigned int out_size) {
    struct termios old_tio;
    struct termios new_tio;
    int have_tio = 0;

    if (!out || out_size == 0) return 0;
    fputs(prompt, stdout);
    fflush(stdout);

    if (tcgetattr(STDIN_FILENO, &old_tio) == 0) {
        new_tio = old_tio;
        new_tio.c_lflag &= ~ECHO;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &new_tio) == 0) {
            have_tio = 1;
        }
    }

    if (!fgets(out, (int)out_size, stdin)) {
        if (have_tio) tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);
        putchar('\n');
        return 0;
    }

    if (have_tio) tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);
    putchar('\n');
    strip_newline(out);
    return 1;
}

static void generate_salt(char* out, unsigned int out_size) {
    static const char alphabet[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-";
    unsigned int seed = (unsigned int)time(0) ^ ((unsigned int)getpid() << 8) ^
                        ((unsigned int)getuid() << 16) ^ (unsigned int)rand();

    if (!out || out_size == 0) return;
    srand(seed);
    for (unsigned int i = 0; i + 1u < out_size && i < PASSWD_SALT_LEN; i++) {
        out[i] = alphabet[(unsigned int)rand() % (sizeof(alphabet) - 1u)];
    }
    out[PASSWD_SALT_LEN < out_size ? PASSWD_SALT_LEN : out_size - 1u] = '\0';
}

static int make_hash(const char* password, char* out, unsigned int out_size) {
    char salt[PASSWD_SALT_LEN + 1u];
    char spec[48];
    char* hash;

    generate_salt(salt, sizeof(salt));
    snprintf(spec, sizeof(spec), "$smallos-sha256$%s$", salt);
    hash = crypt(password, spec);
    if (!hash || hash[0] == '*' || strlen(hash) + 1u > out_size) return 0;
    copy_field(out, out_size, hash);
    return 1;
}

static int write_shadow_line(FILE* out, const char* line, const char* target,
                             const char* new_hash, int* updated) {
    char work[PASSWD_LINE_MAX];
    char* first_colon;
    char* rest;
    char* suffix;

    copy_field(work, sizeof(work), line);
    strip_newline(work);
    first_colon = strchr(work, ':');
    if (!first_colon) {
        return fprintf(out, "%s\n", work) >= 0;
    }
    *first_colon = '\0';
    rest = first_colon + 1;
    suffix = strchr(rest, ':');
    if (strcmp(work, target) != 0) {
        return fprintf(out, "%s\n", line) >= 0;
    }

    if (!suffix) suffix = ":0:0:99999:7:::";
    if (fprintf(out, "%s:%s%s\n", work, new_hash, suffix) < 0) return 0;
    *updated = 1;
    return 1;
}

static int update_shadow(const char* target, const char* new_hash) {
    FILE* in;
    FILE* out;
    char line[PASSWD_LINE_MAX];
    int updated = 0;
    const char* tmp = "/etc/shadow.tmp";
    const char* bak = "/etc/shadow.old";
    int had_old;

    unlink(tmp);
    out = fopen(tmp, "w");
    if (!out) {
        perror("passwd: /etc/shadow.tmp");
        return 0;
    }

    in = fopen("/etc/shadow", "r");
    if (in) {
        while (fgets(line, sizeof(line), in)) {
            strip_newline(line);
            if (!write_shadow_line(out, line, target, new_hash, &updated)) {
                fclose(in);
                fclose(out);
                unlink(tmp);
                return 0;
            }
        }
        fclose(in);
    }

    if (!updated && fprintf(out, "%s:%s:0:0:99999:7:::\n", target, new_hash) < 0) {
        fclose(out);
        unlink(tmp);
        return 0;
    }

    if (fclose(out) < 0) {
        unlink(tmp);
        return 0;
    }
    chmod(tmp, 0600);
    chown(tmp, 0, 0);

    unlink(bak);
    had_old = access("/etc/shadow", F_OK) == 0;
    if (had_old && rename("/etc/shadow", bak) < 0) {
        perror("passwd: backup shadow");
        unlink(tmp);
        return 0;
    }
    if (rename(tmp, "/etc/shadow") < 0) {
        perror("passwd: rename");
        if (had_old) rename(bak, "/etc/shadow");
        unlink(tmp);
        return 0;
    }
    if (had_old) unlink(bak);
    return 1;
}

static int set_password(const char* target) {
    char first[PASSWD_FIELD_MAX];
    char second[PASSWD_FIELD_MAX];
    char hash[PASSWD_HASH_MAX];

    if (!read_password("New password: ", first, sizeof(first))) return 1;
    if (first[0] == '\0') {
        fputs("passwd: empty password rejected\n", stderr);
        return 1;
    }
    if (!read_password("Retype new password: ", second, sizeof(second))) return 1;
    if (strcmp(first, second) != 0) {
        fputs("passwd: passwords do not match\n", stderr);
        return 1;
    }
    if (!make_hash(first, hash, sizeof(hash))) {
        fputs("passwd: failed to hash password\n", stderr);
        return 1;
    }
    if (!update_shadow(target, hash)) {
        fputs("passwd: failed to update shadow\n", stderr);
        return 1;
    }
    puts("passwd: password updated");
    return 0;
}

void _start(int argc, char** argv) {
    char target[PASSWD_FIELD_MAX];
    int clear = 0;

    if (argc >= 2 && strcmp(argv[1], "-d") == 0) {
        if (argc != 3) {
            fputs("usage: passwd [-d user] [user]\n", stderr);
            exit(1);
        }
        if (geteuid() != 0) {
            fputs("passwd: permission denied\n", stderr);
            exit(1);
        }
        clear = 1;
        copy_field(target, sizeof(target), argv[2]);
    } else if (argc == 2) {
        copy_field(target, sizeof(target), argv[1]);
    } else if (argc == 1) {
        if (!current_username(target, sizeof(target))) {
            fputs("passwd: cannot determine current user\n", stderr);
            exit(1);
        }
    } else {
        fputs("usage: passwd [-d user] [user]\n", stderr);
        exit(1);
    }

    if (!passwd_user_exists(target)) {
        fputs("passwd: unknown user\n", stderr);
        exit(1);
    }
    if (geteuid() != 0) {
        char current[PASSWD_FIELD_MAX];
        if (!current_username(current, sizeof(current)) || strcmp(current, target) != 0) {
            fputs("passwd: permission denied\n", stderr);
            exit(1);
        }
    }

    if (clear) {
        if (!update_shadow(target, "")) {
            fputs("passwd: failed to update shadow\n", stderr);
            exit(1);
        }
        puts("passwd: password cleared");
        exit(0);
    }

    exit(set_password(target));
}
