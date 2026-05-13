from .common import case


CASES = [
    case(
        name="ls_path",
        must_contain=[
            "shelltest: ls_path begin",
            "ext2 directory: usr/bin",
            "hello.elf",
            "shelltest: ls_path end",
        ],
        timeout=60.0,
    )
]
