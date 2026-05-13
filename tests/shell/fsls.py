from .common import case


CASES = [
    case(
        name="fsls",
        must_contain=[
            "shelltest: fsls begin",
            "ext2 root directory:",
            "bin/",
            "etc/",
            "tmp/",
            "usr/",
            "var/",
            "shelltest: fsls end",
        ],
        timeout=60.0,
    )
]
