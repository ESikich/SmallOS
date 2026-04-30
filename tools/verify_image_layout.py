#!/usr/bin/env python3

import argparse
import sys
from pathlib import Path


def read_bytes(path: Path) -> bytes:
    return path.read_bytes()


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def u32_le(data: bytes, offset: int) -> int:
    return (
        data[offset + 0]
        | (data[offset + 1] << 8)
        | (data[offset + 2] << 16)
        | (data[offset + 3] << 24)
    )


def patch_u32_le(buf: bytearray, offset: int, value: int) -> None:
    buf[offset + 0] = value & 0xFF
    buf[offset + 1] = (value >> 8) & 0xFF
    buf[offset + 2] = (value >> 16) & 0xFF
    buf[offset + 3] = (value >> 24) & 0xFF


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify the final bootable image layout.")
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--boot", type=Path, required=True)
    parser.add_argument("--loader2", type=Path, required=True)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--fat16", type=Path, required=True)
    parser.add_argument("--sector-size", type=int, required=True)
    parser.add_argument("--boot-loader2-sectors-patch-offset", type=int, required=True)
    parser.add_argument("--boot-fat16-lba-patch-offset", type=int, required=True)
    args = parser.parse_args()

    image = read_bytes(args.image)
    boot = bytearray(read_bytes(args.boot))
    loader2 = read_bytes(args.loader2)
    kernel = read_bytes(args.kernel)
    fat16 = read_bytes(args.fat16)

    expect(len(boot) == args.sector_size, f"boot.bin must be {args.sector_size} bytes")
    expect(len(loader2) == 2048, "loader2.bin must be 2048 bytes")
    expect(len(loader2) % args.sector_size == 0, "loader2.bin must be sector-aligned")
    expect(args.boot_loader2_sectors_patch_offset + 4 <= args.sector_size,
           "boot loader2 sector-count patch field must fit inside the boot sector")
    expect(args.boot_fat16_lba_patch_offset + 4 <= args.sector_size,
           "boot FAT16 patch field must fit inside the boot sector")

    loader2_sectors = len(loader2) // args.sector_size
    kernel_lba = 1 + loader2_sectors
    kernel_padded_size = ((len(kernel) + args.sector_size - 1) // args.sector_size) * args.sector_size
    kernel_pad = kernel_padded_size - len(kernel)
    kernel_sectors = kernel_padded_size // args.sector_size
    fat16_lba = kernel_lba + kernel_sectors

    expected_size = args.sector_size + len(loader2) + kernel_padded_size + len(fat16)
    expect(len(image) == expected_size,
           f"image size mismatch: expected {expected_size}, got {len(image)}")

    patch_u32_le(boot, args.boot_loader2_sectors_patch_offset, loader2_sectors)
    patch_u32_le(boot, args.boot_fat16_lba_patch_offset, fat16_lba)
    expect(image[:len(boot)] == boot, "boot sector bytes do not match patched boot.bin")
    expect(image[args.sector_size:args.sector_size + len(loader2)] == loader2,
           "loader2 bytes do not match loader2.bin")
    expect(image[kernel_lba * args.sector_size:
                 kernel_lba * args.sector_size + len(kernel)] == kernel,
           "kernel bytes do not match kernel.bin at the expected LBA")
    expect(image[kernel_lba * args.sector_size + len(kernel):
                 kernel_lba * args.sector_size + kernel_padded_size] == b"\x00" * kernel_pad,
           "kernel padding bytes are not zero")

    fat16_offset = fat16_lba * args.sector_size
    expect(image[fat16_offset:fat16_offset + len(fat16)] == fat16,
           "fat16 image bytes do not match at the expected LBA")

    patched_lba = u32_le(image, args.boot_fat16_lba_patch_offset)
    expect(patched_lba == fat16_lba,
           f"boot sector FAT16 patch mismatch: expected {fat16_lba}, got {patched_lba}")

    print("image layout ok")
    print(f"  image size          = {len(image)} bytes")
    print(f"  loader2 sectors     = {loader2_sectors}")
    print(f"  kernel start LBA    = {kernel_lba}")
    print(f"  kernel sectors      = {kernel_sectors}")
    print(f"  FAT16 start LBA     = {fat16_lba}")
    print(f"  kernel padded bytes = {kernel_padded_size}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ValueError as exc:
        print(f"image layout check failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
