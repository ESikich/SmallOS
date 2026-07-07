#include "errno.h"
#include "crypt.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "sys/types.h"
#include "termios.h"
#include "unistd.h"

#define LOGIN_LINE_MAX 256
#define LOGIN_FIELD_MAX 96
#define LOGIN_HASH_MAX 128

typedef struct login_account {
    char name[LOGIN_FIELD_MAX];
    char password[LOGIN_FIELD_MAX];
    uid_t uid;
    gid_t gid;
    char gecos[LOGIN_FIELD_MAX];
    char home[LOGIN_FIELD_MAX];
    char shell[LOGIN_FIELD_MAX];
} login_account_t;

typedef struct login_shadow {
    char name[LOGIN_FIELD_MAX];
    char password[LOGIN_HASH_MAX];
} login_shadow_t;

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

static void strip_newline(char* s) {
    if (!s) return;
    for (unsigned int i = 0; s[i]; i++) {
        if (s[i] == '\n' || s[i] == '\r') {
            s[i] = '\0';
            return;
        }
    }
}

static int parse_passwd_line(char* line, login_account_t* account) {
    char* fields[7];
    char* p = line;

    if (!line || !account) return 0;
    for (unsigned int i = 0; i < 7; i++) {
        fields[i] = p;
        if (i == 6) break;
        while (*p && *p != ':') p++;
        if (*p != ':') return 0;
        *p++ = '\0';
    }

    copy_field(account->name, sizeof(account->name), fields[0]);
    copy_field(account->password, sizeof(account->password), fields[1]);
    account->uid = (uid_t)strtoul(fields[2], 0, 10);
    account->gid = (gid_t)strtoul(fields[3], 0, 10);
    copy_field(account->gecos, sizeof(account->gecos), fields[4]);
    copy_field(account->home, sizeof(account->home), fields[5]);
    copy_field(account->shell, sizeof(account->shell), fields[6]);
    if (!account->home[0]) copy_field(account->home, sizeof(account->home), "/");
    if (!account->shell[0]) copy_field(account->shell, sizeof(account->shell), "/bin/shell");
    return account->name[0] != '\0';
}

static int find_account(const char* username, login_account_t* account) {
    FILE* fp;
    char line[LOGIN_LINE_MAX];

    if (!username || !username[0] || !account) return 0;

    fp = fopen("/etc/passwd", "r");
    if (!fp) {
        perror("login: /etc/passwd");
        return 0;
    }

    while (fgets(line, sizeof(line), fp)) {
        login_account_t candidate;
        strip_newline(line);
        if (!parse_passwd_line(line, &candidate)) continue;
        if (strcmp(candidate.name, username) == 0) {
            *account = candidate;
            fclose(fp);
            return 1;
        }
    }

    fclose(fp);
    return 0;
}

static int parse_shadow_line(char* line, login_shadow_t* shadow) {
    char* name;
    char* password;
    char* p;

    if (!line || !shadow) return 0;
    name = line;
    p = strchr(line, ':');
    if (!p) return 0;
    *p++ = '\0';
    password = p;
    p = strchr(password, ':');
    if (p) *p = '\0';

    copy_field(shadow->name, sizeof(shadow->name), name);
    copy_field(shadow->password, sizeof(shadow->password), password);
    return shadow->name[0] != '\0';
}

static int find_shadow(const char* username, login_shadow_t* shadow) {
    FILE* fp;
    char line[LOGIN_LINE_MAX];

    if (!username || !username[0] || !shadow) return 0;

    fp = fopen("/etc/shadow", "r");
    if (!fp) {
        perror("login: /etc/shadow");
        return 0;
    }

    while (fgets(line, sizeof(line), fp)) {
        login_shadow_t candidate;
        strip_newline(line);
        if (!parse_shadow_line(line, &candidate)) continue;
        if (strcmp(candidate.name, username) == 0) {
            *shadow = candidate;
            fclose(fp);
            return 1;
        }
    }

    fclose(fp);
    return 0;
}

static int read_username(char* out, unsigned int out_size) {
    if (!out || out_size == 0) return 0;
    fputs("SmallOS login: ", stdout);
    fflush(stdout);
    if (!fgets(out, (int)out_size, stdin)) return 0;
    strip_newline(out);
    return out[0] != '\0';
}

static int read_password(char* out, unsigned int out_size) {
    struct termios old_tio;
    struct termios new_tio;
    int have_tio = 0;

    if (!out || out_size == 0) return 0;
    fputs("Password: ", stdout);
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

static const char* username_from_args(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (!argv[i]) continue;
        if (strcmp(argv[i], "--") == 0) continue;
        return argv[i];
    }
    return 0;
}

static int login_session(const login_account_t* account) {
    char* shell_argv[2];

    if (setgid(account->gid) < 0) {
        perror("login: setgid");
        return 1;
    }
    if (setuid(account->uid) < 0) {
        perror("login: setuid");
        return 1;
    }

    setenv("HOME", account->home, 1);
    setenv("USER", account->name, 1);
    setenv("LOGNAME", account->name, 1);
    setenv("SHELL", account->shell, 1);
    if (chdir(account->home) < 0) {
        perror("login: chdir");
        chdir("/");
    }

    shell_argv[0] = (char*)account->shell;
    shell_argv[1] = 0;
    execve(account->shell, shell_argv, environ);
    perror("login: exec shell");
    return errno ? errno : 127;
}

static int password_locked(const char* password) {
    return password && (password[0] == '!' || password[0] == '*');
}

static int authenticate_account(const login_account_t* account) {
    const char* stored = account->password;
    login_shadow_t shadow;
    char entered[LOGIN_FIELD_MAX];
    char* hashed;

    if (strcmp(stored, "x") == 0) {
        if (!find_shadow(account->name, &shadow)) return 0;
        stored = shadow.password;
    }

    if (!stored || stored[0] == '\0') return 1;
    if (password_locked(stored)) return 0;
    if (!read_password(entered, sizeof(entered))) return 0;

    hashed = crypt(entered, stored);
    return hashed && strcmp(hashed, stored) == 0;
}

int main(int argc, char** argv) {
    const char* arg_user = username_from_args(argc, argv);
    char username[LOGIN_FIELD_MAX];

    while (1) {
        login_account_t account;
        const char* user = arg_user;

        if (!user) {
            if (!read_username(username, sizeof(username))) {
                putchar('\n');
                continue;
            }
            user = username;
        }

        if (find_account(user, &account) && authenticate_account(&account)) {
            return login_session(&account);
        }

        puts("Login incorrect");
        if (arg_user) return 1;
    }
}
