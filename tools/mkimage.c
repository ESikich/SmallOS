#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t  u8;
typedef uint32_t u32;

/*
 * Final disk image assembly inputs and layout parameters.
 *
 * The Makefile discovers layout constants from the source files that own
 * them (boot.asm, loader2.asm, mkfat16.c) and passes them in here.
 *
 * mkimage itself does not "own" those constants. It just applies them.
 */
typedef struct {
    const char* boot_path;
    const char* loader_path;
    const char* kernel_path;
    const char* fat16_path;
    const char* out_path;
    u32 sector_size;
    u32 loader_size;
    u32 boot_loader2_sectors_patch_offset;
    u32 boot_fat16_lba_patch_offset;
} Options;

/* Fatal error with a simple fixed message. */
static void die(const char* msg) {
    fprintf(stderr, "mkimage: %s\n", msg);
    exit(1);
}

/* Fatal error that includes errno context for file operations. */
static void die_errno(const char* what, const char* path) {
    fprintf(stderr, "mkimage: %s '%s': %s\n", what, path, strerror(errno));
    exit(1);
}

/*
 * Parse a u32 command-line value.
 *
 * Accepts decimal or 0x-prefixed values. Rejects empty, partial, or
 * out-of-range input so image layout math stays explicit and predictable.
 */
static u32 parse_u32(const char* s, const char* name) {
    char* end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (s[0] == '\0' || end == s || *end != '\0') {
        fprintf(stderr, "mkimage: invalid %s: '%s'\n", name, s);
        exit(1);
    }
    if (v > 0xFFFFFFFFul) {
        fprintf(stderr, "mkimage: %s out of range: '%s'\n", name, s);
        exit(1);
    }
    return (u32)v;
}

/*
 * Parse command-line arguments into an Options struct.
 *
 * Required inputs are the already-built component binaries:
 *   - boot.bin
 *   - loader2.bin
 *   - kernel.bin
 *   - fat16.img
 *
 * Required numeric parameters define how those binaries are assembled into
 * the final bootable disk image.
 */
static void parse_args(int argc, char** argv, Options* opt) {
    memset(opt, 0, sizeof(*opt));

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--boot") == 0 && i + 1 < argc) {
            opt->boot_path = argv[++i];
        } else if (strcmp(argv[i], "--loader") == 0 && i + 1 < argc) {
            opt->loader_path = argv[++i];
        } else if (strcmp(argv[i], "--kernel") == 0 && i + 1 < argc) {
            opt->kernel_path = argv[++i];
        } else if (strcmp(argv[i], "--fat16") == 0 && i + 1 < argc) {
            opt->fat16_path = argv[++i];
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            opt->out_path = argv[++i];
        } else if (strcmp(argv[i], "--sector-size") == 0 && i + 1 < argc) {
            opt->sector_size = parse_u32(argv[++i], "sector size");
        } else if (strcmp(argv[i], "--loader-size") == 0 && i + 1 < argc) {
            opt->loader_size = parse_u32(argv[++i], "loader size");
        } else if (strcmp(argv[i], "--boot-loader2-sectors-patch-offset") == 0 && i + 1 < argc) {
            opt->boot_loader2_sectors_patch_offset = parse_u32(argv[++i], "boot loader2 sectors patch offset");
        } else if (strcmp(argv[i], "--boot-fat16-lba-patch-offset") == 0 && i + 1 < argc) {
            opt->boot_fat16_lba_patch_offset = parse_u32(argv[++i], "boot FAT16 LBA patch offset");
        } else {
            fprintf(stderr, "mkimage: unknown or incomplete argument: %s\n", argv[i]);
            exit(1);
        }
    }

    if (!opt->boot_path || !opt->loader_path || !opt->kernel_path ||
        !opt->fat16_path || !opt->out_path) {
        die("missing required path arguments");
    }

    if (opt->sector_size == 0) {
        die("sector size must be non-zero");
    }
    if (opt->loader_size == 0) {
        die("loader size must be non-zero");
    }
    if (opt->loader_size % opt->sector_size != 0) {
        die("loader size must be sector-aligned");
    }

    /*
     * We patch 4-byte little-endian values into the boot sector.
     * Each patch field must fit entirely inside the first sector.
     */
    if (opt->boot_loader2_sectors_patch_offset + 4 > opt->sector_size) {
        die("boot loader2 sectors patch offset does not fit in boot sector");
    }
    if (opt->boot_fat16_lba_patch_offset + 4 > opt->sector_size) {
        die("boot FAT16 LBA patch offset does not fit in boot sector");
    }
}

/*
 * Read a whole file into memory.
 *
 * This is simple and fine here because the inputs are small build artifacts,
 * not arbitrary multi-gigabyte images.
 */
static void read_file(const char* path, u8** out_buf, size_t* out_size) {
    FILE* f = fopen(path, "rb");
    if (!f) die_errno("cannot open", path);

    if (fseek(f, 0, SEEK_END) != 0) die_errno("cannot seek", path);
    long size = ftell(f);
    if (size < 0) die_errno("cannot tell size of", path);
    if (fseek(f, 0, SEEK_SET) != 0) die_errno("cannot seek", path);

    u8* buf = (u8*)malloc((size_t)size);
    if (!buf && size != 0) die("out of memory");

    if (size != 0 && fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        die_errno("cannot read", path);
    }

    fclose(f);
    *out_buf = buf;
    *out_size = (size_t)size;
}

/* Write a full buffer or fail. */
static void write_all(FILE* f, const u8* buf, size_t size, const char* out_path) {
    if (size == 0) return;
    if (fwrite(buf, 1, size, f) != size) {
        die_errno("cannot write", out_path);
    }
}

/*
 * Append zero padding.
 *
 * Used to round kernel.bin up to a whole number of sectors before fat16.img
 * is appended, so FAT16 starts at a clean LBA boundary.
 */
static void write_zeroes(FILE* f, size_t count, const char* out_path) {
    static const u8 zeros[4096] = {0};
    while (count > 0) {
        size_t chunk = count < sizeof(zeros) ? count : sizeof(zeros);
        if (fwrite(zeros, 1, chunk, f) != chunk) {
            die_errno("cannot write", out_path);
        }
        count -= chunk;
    }
}

/*
 * Patch a 32-bit little-endian value into an in-memory buffer.
 *
 * Here this is used to write the FAT16 start LBA into the boot sector field
 * declared by boot.asm.
 */
static void patch_u32_le(u8* buf, u32 offset, u32 value) {
    buf[offset + 0] = (u8)(value & 0xFF);
    buf[offset + 1] = (u8)((value >> 8) & 0xFF);
    buf[offset + 2] = (u8)((value >> 16) & 0xFF);
    buf[offset + 3] = (u8)((value >> 24) & 0xFF);
}

int main(int argc, char** argv) {
    Options opt;
    parse_args(argc, argv, &opt);

    u8 *boot = NULL, *loader = NULL, *kernel = NULL, *fat16 = NULL;
    size_t boot_size = 0, loader_size = 0, kernel_size = 0, fat16_size = 0;

    read_file(opt.boot_path, &boot, &boot_size);
    read_file(opt.loader_path, &loader, &loader_size);
    read_file(opt.kernel_path, &kernel, &kernel_size);
    read_file(opt.fat16_path, &fat16, &fat16_size);

    /*
     * Validate fixed-layout invariants before building the final image.
     *
     * boot.bin must occupy exactly one sector.
     * loader2.bin must match the size contract owned by loader2.asm.
     */
    if (boot_size != opt.sector_size) {
        fprintf(stderr, "mkimage: boot image must be %u bytes, got %zu\n",
                opt.sector_size, boot_size);
        return 1;
    }
    if (loader_size != opt.loader_size) {
        fprintf(stderr, "mkimage: loader image must be %u bytes, got %zu\n",
                opt.loader_size, loader_size);
        return 1;
    }

    /*
     * Final image layout:
     *   [boot.bin]
     *   [loader2.bin]
     *   [kernel.bin]
     *   [zero padding to next sector boundary]
     *   [fat16.img]
     *
     * FAT16 starts immediately after the padded kernel region.
     */
    size_t padded_kernel_size =
        ((kernel_size + opt.sector_size - 1) / opt.sector_size) * opt.sector_size;
    size_t kernel_pad = padded_kernel_size - kernel_size;
    u32 kernel_sectors = (u32)(padded_kernel_size / opt.sector_size);

    u32 loader_sectors = (u32)(opt.loader_size / opt.sector_size);
    u32 kernel_lba = 1u + loader_sectors;

    if (kernel_sectors > 0xFFFFFFFFu - kernel_lba) {
        die("computed FAT16 LBA overflows u32");
    }
    u32 fat16_lba = kernel_lba + kernel_sectors;

    /* Patch loader2 size and FAT16 start LBA into the boot sector before writing output. */
    patch_u32_le(boot, opt.boot_loader2_sectors_patch_offset, loader_sectors);
    patch_u32_le(boot, opt.boot_fat16_lba_patch_offset, fat16_lba);

    FILE* out = fopen(opt.out_path, "wb");
    if (!out) die_errno("cannot open output", opt.out_path);

    write_all(out, boot, boot_size, opt.out_path);
    write_all(out, loader, loader_size, opt.out_path);
    write_all(out, kernel, kernel_size, opt.out_path);
    write_zeroes(out, kernel_pad, opt.out_path);
    write_all(out, fat16, fat16_size, opt.out_path);

    if (fclose(out) != 0) {
        die_errno("cannot close output", opt.out_path);
    }

    /*
     * Reporting is based on actual artifact sizes, not duplicated constants.
     * That keeps the output honest even if FAT image size changes later.
     */
    size_t fat16_sectors = fat16_size / opt.sector_size;

    printf("kernel:  %zu bytes (%u sectors, LBA %u)\n",
           kernel_size, kernel_sectors, kernel_lba);
    printf("fat16:   %zu sectors (%zu MB), LBA %u\n",
           fat16_sectors,
           fat16_size / (1024u * 1024u),
           fat16_lba);
    printf("fat16:   LBA %u patched into sector 0 offset %u\n",
           fat16_lba, opt.boot_fat16_lba_patch_offset);
    printf("boot:    loader2 sectors %u patched into sector 0 offset %u\n",
           loader_sectors, opt.boot_loader2_sectors_patch_offset);

    free(boot);
    free(loader);
    free(kernel);
    free(fat16);
    return 0;
}
