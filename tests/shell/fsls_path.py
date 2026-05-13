from .common import case


CASES = [
    case(
        name="fsls_path",
        must_contain=[
            "shelltest: fsls_path begin",
            "ext2 directory: usr/bin",
            "hello.elf",
            "shelltest: fsls_path end",
        ],
        timeout=60.0,
    )
]
