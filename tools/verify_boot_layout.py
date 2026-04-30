#!/usr/bin/env python3

import argparse
import re
import sys
from pathlib import Path


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def parse_int_literal(token: str) -> int:
    return int(token.rstrip("uUlL"), 0)


def parse_equ(text: str, name: str) -> int:
    patterns = [
        rf"^{re.escape(name)}[ \t]+equ[ \t]+([^\s;]+)",
        rf"^%define[ \t]+{re.escape(name)}[ \t]+([^\s;]+)",
        rf"^#define[ \t]+{re.escape(name)}[ \t]+([^\s;]+)",
    ]
    for pattern in patterns:
        m = re.search(pattern, text, re.MULTILINE)
        if m:
            token = m.group(1)
            return parse_int_literal(token)
    raise ValueError(f"missing constant: {name}")


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify the custom boot layout contract.")
    parser.add_argument("--boot-asm", type=Path, required=True)
    parser.add_argument("--loader2-asm", type=Path, required=True)
    parser.add_argument("--memory-h", type=Path, required=True)
    parser.add_argument("--boot-bin", type=Path, required=True)
    parser.add_argument("--loader2-bin", type=Path, required=True)
    parser.add_argument("--kernel-bin", type=Path, required=True)
    parser.add_argument("--loader2-gen", type=Path, required=True)
    args = parser.parse_args()

    boot_asm = read_text(args.boot_asm)
    loader2_asm = read_text(args.loader2_asm)
    memory_h = read_text(args.memory_h)
    loader2_gen = read_text(args.loader2_gen)

    boot_sector_size = parse_equ(boot_asm, "BOOT_SECTOR_SIZE")
    loader2_segment = parse_equ(boot_asm, "LOADER2_SEGMENT")
    loader2_offset = parse_equ(boot_asm, "LOADER2_OFFSET")
    kernel_offset = parse_equ(loader2_asm, "KERNEL_OFFSET")
    stage2_stack_top = parse_equ(loader2_gen, "STAGE2_STACK_TOP")
    stage2_stack_top_32 = parse_equ(loader2_gen, "STAGE2_STACK_TOP_32")
    kernel_boot_stack_top = parse_equ(memory_h, "KERNEL_BOOT_STACK_TOP")

    loader2_load_addr = (loader2_segment << 4) + loader2_offset
    stage2_stack_top_phys = 0x1FF00
    expected_stage2_stack_top = 0xFF00
    expected_stage2_stack_top_32 = 0x1FF000
    expected_kernel_boot_stack_top = 0x1FF000
    kernel_sectors = (args.kernel_bin.stat().st_size + boot_sector_size - 1) // boot_sector_size
    safe_sectors = min(
        (stage2_stack_top_phys - kernel_offset) // boot_sector_size,
        (loader2_load_addr - kernel_offset) // boot_sector_size,
    )

    expect(boot_sector_size == 512, f"BOOT_SECTOR_SIZE must be 512, got {boot_sector_size}")
    expect(loader2_load_addr == 0x10000, f"loader2 must load at 0x10000, got 0x{loader2_load_addr:x}")
    expect(kernel_offset == 0x1000, f"KERNEL_OFFSET must be 0x1000, got 0x{kernel_offset:x}")
    expect(kernel_boot_stack_top == expected_kernel_boot_stack_top,
           f"KERNEL_BOOT_STACK_TOP must be 0x{expected_kernel_boot_stack_top:x}, got 0x{kernel_boot_stack_top:x}")
    expect(stage2_stack_top is None or stage2_stack_top == expected_stage2_stack_top,
           f"stage2 stack top must be 0x{expected_stage2_stack_top:x}")
    expect(stage2_stack_top_32 is None or stage2_stack_top_32 == expected_stage2_stack_top_32,
           f"stage2 32-bit stack top must be 0x{expected_stage2_stack_top_32:x}")
    expect(kernel_sectors <= safe_sectors,
           f"kernel needs {kernel_sectors} sectors, but the boot layout only allows {safe_sectors}")
    boot_bin_size = args.boot_bin.stat().st_size
    loader2_bin_size = args.loader2_bin.stat().st_size

    expect(boot_bin_size == 512, f"boot.bin must be 512 bytes, got {boot_bin_size}")
    expect(loader2_bin_size == 2048, f"loader2.bin must be 2048 bytes, got {loader2_bin_size}")
    expect("jmp dword CODE_SEG:init_pm" in loader2_asm,
           "loader2.asm must use a 32-bit far jump into init_pm")
    expect("[org 0x10000]" in loader2_asm,
           "loader2.asm must be assembled with org 0x10000")

    print("boot layout ok")
    print(f"  boot sector size    = {boot_sector_size}")
    print(f"  loader2 load addr   = 0x{loader2_load_addr:x}")
    print(f"  kernel offset       = 0x{kernel_offset:x}")
    print(f"  kernel size         = {kernel_sectors} sectors")
    print(f"  safe kernel ceiling = {safe_sectors} sectors")
    print(f"  stage2 stack top    = 0x{stage2_stack_top:x} (phys 0x{stage2_stack_top_phys:x})")
    print(f"  kernel boot stack   = 0x{kernel_boot_stack_top:x}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ValueError as exc:
        print(f"boot layout check failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
