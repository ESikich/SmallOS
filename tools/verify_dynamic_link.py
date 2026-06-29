#!/usr/bin/env python3
import argparse
import os
import struct
import sys


PT_DYNAMIC = 2
PT_INTERP = 3

ET_EXEC = 2
ET_DYN = 3

DT_NULL = 0
DT_NEEDED = 1
DT_STRTAB = 5
DT_STRSZ = 10
DT_SONAME = 14
DT_RPATH = 15
DT_RUNPATH = 29

SHT_RELA = 4
SHT_REL = 9

R_386_NONE = 0
R_386_RELATIVE = 8

R_386_NAMES = {
    R_386_NONE: "R_386_NONE",
    1: "R_386_32",
    2: "R_386_PC32",
    5: "R_386_COPY",
    6: "R_386_GLOB_DAT",
    7: "R_386_JMP_SLOT",
    R_386_RELATIVE: "R_386_RELATIVE",
}
SUPPORTED_RELOCS = set(R_386_NAMES)


class ElfError(Exception):
    pass


class Elf32:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.data = f.read()
        self._parse_header()
        self._parse_program_headers()
        self._parse_section_headers()

    def _need(self, offset, size, what):
        if offset < 0 or offset + size > len(self.data):
            raise ElfError(f"{self.path}: truncated {what}")

    def _parse_header(self):
        self._need(0, 52, "ELF header")
        ident = self.data[:16]
        if ident[:4] != b"\x7fELF":
            raise ElfError(f"{self.path}: not an ELF file")
        if ident[4] != 1 or ident[5] != 1:
            raise ElfError(f"{self.path}: expected ELF32 little-endian")
        fields = struct.unpack_from("<HHIIIIIHHHHHH", self.data, 16)
        (
            self.e_type,
            self.e_machine,
            self.e_version,
            self.e_entry,
            self.e_phoff,
            self.e_shoff,
            self.e_flags,
            self.e_ehsize,
            self.e_phentsize,
            self.e_phnum,
            self.e_shentsize,
            self.e_shnum,
            self.e_shstrndx,
        ) = fields
        if self.e_machine != 3:
            raise ElfError(f"{self.path}: expected i386 ELF")

    def _parse_program_headers(self):
        self.phdrs = []
        if self.e_phoff == 0:
            return
        if self.e_phentsize < 32:
            raise ElfError(f"{self.path}: invalid program-header entry size")
        for i in range(self.e_phnum):
            off = self.e_phoff + i * self.e_phentsize
            self._need(off, 32, "program header")
            ph = struct.unpack_from("<IIIIIIII", self.data, off)
            self.phdrs.append(
                {
                    "type": ph[0],
                    "offset": ph[1],
                    "vaddr": ph[2],
                    "paddr": ph[3],
                    "filesz": ph[4],
                    "memsz": ph[5],
                    "flags": ph[6],
                    "align": ph[7],
                }
            )

    def _parse_section_headers(self):
        self.shdrs = []
        if self.e_shoff == 0:
            return
        if self.e_shentsize < 40:
            raise ElfError(f"{self.path}: invalid section-header entry size")
        for i in range(self.e_shnum):
            off = self.e_shoff + i * self.e_shentsize
            self._need(off, 40, "section header")
            sh = struct.unpack_from("<IIIIIIIIII", self.data, off)
            self.shdrs.append(
                {
                    "name": sh[0],
                    "type": sh[1],
                    "flags": sh[2],
                    "addr": sh[3],
                    "offset": sh[4],
                    "size": sh[5],
                    "link": sh[6],
                    "info": sh[7],
                    "addralign": sh[8],
                    "entsize": sh[9],
                }
            )

    def interp(self):
        for ph in self.phdrs:
            if ph["type"] == PT_INTERP:
                self._need(ph["offset"], ph["filesz"], "interpreter")
                return self.data[ph["offset"] : ph["offset"] + ph["filesz"]].rstrip(b"\0").decode("ascii")
        return None

    def has_dynamic_segment(self):
        return any(ph["type"] == PT_DYNAMIC for ph in self.phdrs)

    def vaddr_to_offset(self, vaddr, size=1):
        for ph in self.phdrs:
            if ph["type"] != 1:
                continue
            if vaddr < ph["vaddr"]:
                continue
            if size > ph["filesz"]:
                continue
            delta = vaddr - ph["vaddr"]
            if delta > ph["filesz"] - size:
                continue
            return ph["offset"] + delta
        raise ElfError(f"{self.path}: dynamic virtual address 0x{vaddr:x} is not file-backed")

    def dynamic_entries(self):
        for ph in self.phdrs:
            if ph["type"] != PT_DYNAMIC:
                continue
            if ph["filesz"] % 8 != 0:
                raise ElfError(f"{self.path}: invalid PT_DYNAMIC size")
            entries = []
            for i in range(ph["filesz"] // 8):
                off = ph["offset"] + i * 8
                self._need(off, 8, "dynamic entry")
                tag, val = struct.unpack_from("<iI", self.data, off)
                entries.append((tag, val))
                if tag == DT_NULL:
                    break
            return entries
        return []

    def dynamic_string_table(self):
        strtab = None
        strsz = None
        for tag, val in self.dynamic_entries():
            if tag == DT_STRTAB:
                strtab = val
            elif tag == DT_STRSZ:
                strsz = val
        if strtab is None:
            return None, 0
        if strsz is None:
            strsz = 1
        return self.vaddr_to_offset(strtab, 1), strsz

    def dynamic_string(self, dyn_offset):
        strtab_off, strsz = self.dynamic_string_table()
        if strtab_off is None:
            raise ElfError(f"{self.path}: dynamic string requested without DT_STRTAB")
        off = strtab_off + dyn_offset
        if off >= len(self.data):
            raise ElfError(f"{self.path}: dynamic string offset out of range")
        limit = min(len(self.data), strtab_off + strsz)
        end = off
        while end < limit and self.data[end] != 0:
            end += 1
        if end >= limit:
            raise ElfError(f"{self.path}: unterminated dynamic string")
        return self.data[off:end].decode("ascii")

    def dynamic_strings(self, wanted_tag):
        values = []
        for tag, val in self.dynamic_entries():
            if tag == wanted_tag:
                values.append(self.dynamic_string(val))
        return values

    def relocs(self):
        found = []
        for sh in self.shdrs:
            if sh["type"] not in (SHT_REL, SHT_RELA):
                continue
            entsize = sh["entsize"]
            default_size = 8 if sh["type"] == SHT_REL else 12
            if entsize == 0:
                entsize = default_size
            if entsize < default_size or sh["size"] % entsize != 0:
                raise ElfError(f"{self.path}: invalid relocation section")
            count = sh["size"] // entsize
            for i in range(count):
                off = sh["offset"] + i * entsize
                self._need(off, default_size, "relocation")
                r_offset, r_info = struct.unpack_from("<II", self.data, off)
                found.append((r_offset, r_info & 0xFF))
        return found


def fail(errors, msg):
    errors.append(msg)
    print(f"dynamic-link-check: FAIL: {msg}", file=sys.stderr)


def check_relocs(errors, elf):
    for offset, reloc_type in elf.relocs():
        if reloc_type not in SUPPORTED_RELOCS:
            fail(
                errors,
                f"{elf.path}: unsupported relocation type {reloc_type} at 0x{offset:x}",
            )


def check_dynamic_paths(errors, elf):
    for tag, label in ((DT_RPATH, "DT_RPATH"), (DT_RUNPATH, "DT_RUNPATH")):
        try:
            values = elf.dynamic_strings(tag)
        except ElfError as exc:
            fail(errors, str(exc))
            continue
        for value in values:
            if "$" in value:
                fail(errors, f"{elf.path}: {label} uses unsupported token in {value!r}")
                continue
            for entry in value.split(":"):
                if entry and not entry.startswith("/"):
                    fail(errors, f"{elf.path}: {label} contains non-absolute path {entry!r}")


def check_soname(errors, elf, path):
    sonames = elf.dynamic_strings(DT_SONAME)
    if len(sonames) != 1:
        fail(errors, f"{path}: expected exactly one DT_SONAME, got {len(sonames)}")
        return
    expected = os.path.basename(path)
    if sonames[0] != expected:
        fail(errors, f"{path}: expected DT_SONAME={expected}, got {sonames[0]!r}")


def check_executable(errors, path, expected_interp):
    try:
        elf = Elf32(path)
        if elf.e_type != ET_EXEC:
            fail(errors, f"{path}: expected ET_EXEC dynamic executable")
        interp = elf.interp()
        if interp != expected_interp:
            fail(errors, f"{path}: expected PT_INTERP={expected_interp}, got {interp!r}")
        if not elf.has_dynamic_segment():
            fail(errors, f"{path}: missing PT_DYNAMIC")
        check_relocs(errors, elf)
        check_dynamic_paths(errors, elf)
    except (OSError, ElfError) as exc:
        fail(errors, str(exc))


def check_shared(errors, path):
    try:
        elf = Elf32(path)
        if elf.e_type != ET_DYN:
            fail(errors, f"{path}: expected ET_DYN shared object")
        if not elf.has_dynamic_segment():
            fail(errors, f"{path}: missing PT_DYNAMIC")
        check_relocs(errors, elf)
        check_dynamic_paths(errors, elf)
        check_soname(errors, elf, path)
    except (OSError, ElfError) as exc:
        fail(errors, str(exc))


def check_loader(errors, path):
    try:
        elf = Elf32(path)
        if elf.e_type != ET_DYN:
            fail(errors, f"{path}: expected ET_DYN self-relocating ELF loader")
        if elf.interp() is not None:
            fail(errors, f"{path}: loader must not have PT_INTERP")
        if not elf.has_dynamic_segment():
            fail(errors, f"{path}: missing PT_DYNAMIC")
        load_vaddrs = [ph["vaddr"] for ph in elf.phdrs if ph["type"] == 1]
        if not load_vaddrs or min(load_vaddrs) >= 0x01000000:
            fail(errors, f"{path}: loader is not linked as a base-zero ET_DYN image")
        for offset, reloc_type in elf.relocs():
            if reloc_type not in (R_386_NONE, R_386_RELATIVE):
                fail(
                    errors,
                    f"{path}: unsupported loader self-relocation type {reloc_type} at 0x{offset:x}",
                )
    except (OSError, ElfError) as exc:
        fail(errors, str(exc))


def main():
    parser = argparse.ArgumentParser(description="Verify SmallOS dynamic-link artifacts")
    parser.add_argument("--interp", default="/lib/ld-smallos.so")
    parser.add_argument("--libc", required=True)
    parser.add_argument("--loader", required=True)
    parser.add_argument("--shared", action="append", default=[])
    parser.add_argument("executables", nargs="+")
    args = parser.parse_args()

    errors = []
    if not os.path.exists(args.libc):
        fail(errors, f"{args.libc}: missing shared runtime")
    else:
        check_shared(errors, args.libc)
    if not os.path.exists(args.loader):
        fail(errors, f"{args.loader}: missing dynamic loader")
    else:
        check_loader(errors, args.loader)
    for shared in args.shared:
        if not os.path.exists(shared):
            fail(errors, f"{shared}: missing shared object")
        else:
            check_shared(errors, shared)
    for exe in args.executables:
        check_executable(errors, exe, args.interp)

    if errors:
        print(f"dynamic-link-check: FAIL ({len(errors)} issue(s))", file=sys.stderr)
        return 1
    print(f"dynamic-link-check: PASS ({len(args.executables)} executables)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
