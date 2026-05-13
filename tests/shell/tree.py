from .common import case


CASES = [
    case(
        name="tree",
        must_contain=[
            "shelltest: tree begin",
            "/",
            "|-- bin/",
            "|-- usr/",
            "|   |-- bin/",
            "|   |   |-- hello.elf",
            "`-- var/",
            "directories, ",
            " files",
            "shelltest: tree end",
        ],
        timeout=60.0,
    ),
    case(
        name="tree_path",
        must_contain=[
            "shelltest: tree_path begin",
            "usr/bin",
            "|-- hello.elf",
            "|-- mandel.elf",
            "|-- plasma.elf",
            "shelltest: tree_path end",
        ],
        timeout=60.0,
    ),
]
