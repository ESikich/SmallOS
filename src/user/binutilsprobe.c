#include "fcntl.h"
#include "stdio.h"
#include "string.h"
#include "sys/wait.h"
#include "unistd.h"

#define OUT_PATH "/tmp/binutilsprobe.out"
#define SRC_PATH "/tmp/bu.s"
#define MSG_PATH "/tmp/bu.msg"
#define OBJ_PATH "/tmp/bu.o"
#define OBJ_GLOBAL_PATH "/tmp/bug.o"
#define MSG_OBJ_PATH "/tmp/bumsg.o"
#define ARCHIVE_PATH "/tmp/bu.a"
#define ELF_PATH "/tmp/bu.elf"
#define COPY_PATH "/tmp/bu.copy"
#define STRIP_PATH "/tmp/bu.strip"

static char output[8192];

static const char marker_text[] = "binutils roundtrip ok\n";

static const char source_text[] =
    "_start:\n"
    "main:\n"
    "    movl $1, %eax\n"
    "    movl $_binary__tmp_bu_msg_start, %ebx\n"
    "    movl $22, %ecx\n"
    "    int $0x80\n"
    "    movl $2, %eax\n"
    "    xorl %ebx, %ebx\n"
    "    int $0x80\n";

static int write_file(const char* path, const char* data) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int len = (int)strlen(data);
    int done = 0;

    if (fd < 0) return -1;
    while (done < len) {
        int n = write(fd, data + done, (unsigned int)(len - done));
        if (n <= 0) {
            close(fd);
            return -1;
        }
        done += n;
    }
    return close(fd);
}

static int read_file(const char* path, char* buf, int cap) {
    int fd;
    int total = 0;

    if (cap <= 0) return -1;
    fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    while (total + 1 < cap) {
        int n = read(fd, buf + total, (unsigned int)(cap - total - 1));
        if (n < 0) {
            close(fd);
            return -1;
        }
        if (n == 0) break;
        total += n;
    }
    close(fd);
    buf[total] = 0;
    return total;
}

static int contains(const char* haystack, const char* needle) {
    return strstr(haystack, needle) != 0;
}

static int run_capture(char* const argv[], int* out_status) {
    int status = 0;
    int fd;
    pid_t pid;

    unlink(OUT_PATH);
    pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        fd = open(OUT_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) return 97;
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
        execve(argv[0], argv, 0);
        return 98;
    }

    if (waitpid(pid, &status, 0) != pid) return -1;
    if (out_status) *out_status = status;
    return read_file(OUT_PATH, output, (int)sizeof(output));
}

static int run_expect_zero(const char* label, char* const argv[]) {
    int status = 0;
    int n = run_capture(argv, &status);
    if (n < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("binutilsprobe %s: FAIL status=%d read=%d\n", label, status, n);
        if (n > 0) {
            puts(output);
        }
        return 0;
    }
    printf("binutilsprobe %s: PASS\n", label);
    return 1;
}

static int run_expect_zero_silent(const char* label, char* const argv[]) {
    int status = 0;
    int n = run_capture(argv, &status);
    if (n < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("binutilsprobe %s: FAIL status=%d read=%d\n", label, status, n);
        if (n > 0) {
            puts(output);
        }
        return 0;
    }
    return 1;
}

static int run_expect_two_zero(const char* label,
                               char* const first[],
                               char* const second[]) {
    int status = 0;
    int n = run_capture(first, &status);
    if (n < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("binutilsprobe %s: FAIL status=%d read=%d\n", label, status, n);
        if (n > 0) {
            puts(output);
        }
        return 0;
    }

    n = run_capture(second, &status);
    if (n < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("binutilsprobe %s: FAIL status=%d read=%d\n", label, status, n);
        if (n > 0) {
            puts(output);
        }
        return 0;
    }

    printf("binutilsprobe %s: PASS\n", label);
    return 1;
}

static int run_expect_output(const char* label,
                             char* const argv[],
                             const char* needle1,
                             const char* needle2) {
    int status = 0;
    int n;

    n = run_capture(argv, &status);
    if (n < 0 ||
        !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0 ||
        !contains(output, needle1) ||
        (needle2 && !contains(output, needle2))) {
        printf("binutilsprobe %s: FAIL status=%d read=%d\n", label, status, n);
        if (n > 0) {
            puts(output);
        }
        return 0;
    }
    printf("binutilsprobe %s: PASS\n", label);
    return 1;
}

static int run_elf(const char* label, const char* path) {
    char* const argv[] = { (char*)path, 0 };
    return run_expect_output(label, argv, "binutils roundtrip ok", 0);
}

int main(void) {
    int ok = 1;

    unlink(SRC_PATH);
    unlink(MSG_PATH);
    unlink(OBJ_PATH);
    unlink(OBJ_GLOBAL_PATH);
    unlink(MSG_OBJ_PATH);
    unlink(ARCHIVE_PATH);
    unlink(ELF_PATH);
    unlink(COPY_PATH);
    unlink(STRIP_PATH);

    puts("binutilsprobe start");
    if (write_file(SRC_PATH, source_text) < 0) {
        puts("binutilsprobe write source: FAIL");
        return 1;
    }
    if (write_file(MSG_PATH, marker_text) < 0) {
        puts("binutilsprobe write source: FAIL");
        return 1;
    }
    puts("binutilsprobe write source: PASS");

    {
        char* const argv[] = {
            "/usr/bin/as", "-o", OBJ_PATH, SRC_PATH, 0
        };
        ok &= run_expect_zero("as", argv);
    }
    {
        char* const argv[] = {
            "/usr/bin/objcopy", "--globalize-symbol=main", OBJ_PATH, OBJ_GLOBAL_PATH, 0
        };
        ok &= run_expect_zero_silent("prepare symbols", argv);
    }
    {
        char* const argv[] = {
            "/usr/bin/ar", "--target=elf32-i386", "-rcS", ARCHIVE_PATH, OBJ_GLOBAL_PATH, 0
        };
        ok &= run_expect_zero("ar", argv);
    }
    {
        char* const argv[] = {
            "/usr/bin/ranlib", "--target=elf32-i386", ARCHIVE_PATH, 0
        };
        ok &= run_expect_zero("ranlib", argv);
    }
    {
        char* const marker_argv[] = {
            "/usr/bin/ld", "-m", "elf_i386",
            "-r",
            "-b", "binary",
            MSG_PATH,
            "-o", MSG_OBJ_PATH,
            0
        };
        char* const link_argv[] = {
            "/usr/bin/ld", "-m", "elf_i386",
            "-Ttext", "4194304",
            "-e", "_start",
            OBJ_GLOBAL_PATH,
            MSG_OBJ_PATH,
            "-o", ELF_PATH,
            0
        };
        ok &= run_expect_two_zero("ld", marker_argv, link_argv);
    }

    ok &= run_elf("run linked", ELF_PATH);

    {
        char* const argv[] = {
            "/usr/bin/readelf", "-h", ELF_PATH, 0
        };
        ok &= run_expect_output("readelf", argv, "ELF Header:", "Intel 80386");
    }
    {
        char* const argv[] = {
            "/usr/bin/objdump", "-f", ELF_PATH, 0
        };
        ok &= run_expect_output("objdump", argv, "file format elf32-i386", 0);
    }
    {
        char* const argv[] = {
            "/usr/bin/nm", ELF_PATH, 0
        };
        ok &= run_expect_output("nm", argv, " main", 0);
    }
    {
        char* const argv[] = {
            "/usr/bin/size", ELF_PATH, 0
        };
        ok &= run_expect_output("size", argv, "filename", "bu.elf");
    }
    {
        char* const argv[] = {
            "/usr/bin/strings", ELF_PATH, 0
        };
        ok &= run_expect_output("strings", argv, "binutils roundtrip ok", 0);
    }
    {
        char* const argv[] = {
            "/usr/bin/addr2line", "-e", ELF_PATH, "0x400000", 0
        };
        ok &= run_expect_zero("addr2line", argv);
    }
    {
        char* const argv[] = {
            "/usr/bin/objcopy", ELF_PATH, COPY_PATH, 0
        };
        ok &= run_expect_zero("objcopy", argv);
    }

    ok &= run_elf("run objcopy", COPY_PATH);

    {
        char* const argv[] = {
            "/usr/bin/strip", "-o", STRIP_PATH, COPY_PATH, 0
        };
        ok &= run_expect_zero("strip", argv);
    }

    ok &= run_elf("run stripped", STRIP_PATH);

    if (!ok) {
        puts("binutilsprobe: FAIL");
        return 1;
    }

    puts("binutilsprobe: PASS");
    return 0;
}
