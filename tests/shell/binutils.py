from .common import case


CASES = [
    case(
        name="binutils_smoke",
        must_contain=[
            "shelltest: binutils_as begin",
            "GNU assembler",
            "shelltest: binutils_as end",
            "shelltest: binutils_ld begin",
            "GNU ld",
            "shelltest: binutils_ld end",
            "shelltest: binutils_ar begin",
            "GNU ar",
            "shelltest: binutils_ar end",
            "shelltest: binutils_readelf begin",
            "ELF Header:",
            "Machine:",
            "Intel 80386",
            "shelltest: binutils_readelf end",
            "shelltest: binutils_objdump begin",
            "file format elf32-i386",
            "shelltest: binutils_objdump end",
            "shelltest: binutils_size begin",
            "filename",
            "shelltest: binutils_size end",
            "shelltest: binutils_nm begin",
            "shelltest: binutils_nm end",
        ],
        timeout=120.0,
    ),
]
