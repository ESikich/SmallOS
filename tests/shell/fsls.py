from .common import case


CASES = [
    case(
        name="fsls",
        must_contain=[
            "shelltest: fsls begin",
            "ext2 root directory:",
            "apps/",
            "bin/",
            "tools/",
            "shelltest: fsls end",
        ],
        timeout=60.0,
    )
]
