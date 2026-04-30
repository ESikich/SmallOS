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


def write_partition_entry(buf: bytearray, offset: int, bootable: int, ptype: int,
                          lba_start: int, sectors: int) -> None:
    buf[offset + 0] = bootable
    buf[offset + 1] = 0
    buf[offset + 2] = 0
    buf[offset + 3] = 0
    buf[offset + 4] = ptype
    buf[offset + 5] = 0
    buf[offset + 6] = 0
    buf[offset + 7] = 0
    patch_u32_le(buf, offset + 8, lba_start)
    patch_u32_le(buf, offset + 12, sectors)


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify the final bootable image layout.")
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--boot", type=Path, required=True)
    parser.add_argument("--loader2", type=Path, required=True)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--fat16", type=Path, required=True)
    parser.add_argument("--sector-size", type=int, required=True)
    parser.add_argument("--boot-partition-table-offset", type=int, required=True)
    parser.add_argument("--boot-partition-entry-size", type=int, required=True)
    args = parser.parse_args()

    image = read_bytes(args.image)
    boot = bytearray(read_bytes(args.boot))
    loader2 = read_bytes(args.loader2)
    kernel = read_bytes(args.kernel)
    fat16 = read_bytes(args.fat16)

    expect(len(boot) == args.sector_size, f"boot.bin must be {args.sector_size} bytes")
    expect(len(loader2) == 2048, "loader2.bin must be 2048 bytes")
    expect(len(loader2) % args.sector_size == 0, "loader2.bin must be sector-aligned")
    expect(args.boot_partition_table_offset + 2 * args.boot_partition_entry_size <= args.sector_size,
           "boot partition table must fit inside the boot sector")

    loader2_sectors = len(loader2) // args.sector_size
    kernel_lba = 1 + loader2_sectors
    kernel_padded_size = ((len(kernel) + args.sector_size - 1) // args.sector_size) * args.sector_size
    kernel_pad = kernel_padded_size - len(kernel)
    kernel_sectors = kernel_padded_size // args.sector_size
    fat16_lba = kernel_lba + kernel_sectors
    fat16_sectors = len(fat16) // args.sector_size

    expected_size = args.sector_size + len(loader2) + kernel_padded_size + len(fat16)
    expect(len(image) == expected_size,
           f"image size mismatch: expected {expected_size}, got {len(image)}")

    write_partition_entry(boot, args.boot_partition_table_offset + 0 * args.boot_partition_entry_size,
                          0x80, 0x83, kernel_lba, kernel_sectors)
    write_partition_entry(boot, args.boot_partition_table_offset + 1 * args.boot_partition_entry_size,
                          0x00, 0x06, fat16_lba, fat16_sectors)
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
