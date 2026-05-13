from .common import case


CASES = [
    case(
        name="ls_abs_root",
        must_contain=[
            "shelltest: ls_abs_root begin",
            "ext2 root directory:",
            "bin/",
            "etc/",
            "tmp/",
            "usr/",
            "var/",
            "shelltest: ls_abs_root end",
        ],
        timeout=60.0,
    )
]
