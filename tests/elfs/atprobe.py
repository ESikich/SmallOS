from .common import case


CASES = [
    case(
        name="atprobe",
        command="usr/libexec/tests/atprobe",
        must_contain=[
            "atprobe start",
            "open dirfd: PASS",
            "openat create: PASS",
            "openat nondir base: PASS",
            "fstatat nondir base: PASS",
            "openat absolute override: PASS",
            "mkdirat dir: PASS",
            "renameat into subdir: PASS",
            "linkat hard: PASS",
            "symlinkat create: PASS",
            "readlinkat target: PASS",
            "fstatat nofollow: PASS",
            "utimensat nofollow: PASS",
            "unlinkat removedir: PASS",
            "atprobe PASS",
        ],
    )
]
