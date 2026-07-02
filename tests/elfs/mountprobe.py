from .common import case


CASES = [
    case(
        "mountprobe",
        "usr/libexec/tests/mountprobe",
        must_contain=[
            "mountprobe start",
            "mounts root line: PASS",
            "mounts proc line: PASS",
            "mounts dev line: PASS",
            "statfs root ext2: PASS",
            "statfs proc: PASS",
            "statfs dev: PASS",
            "fstatfs proc: PASS",
            "fstatfs dev: PASS",
            "mount invalid type: PASS",
            "mount root busy: PASS",
            "mount dynamic gated: PASS",
            "umount root busy: PASS",
            "umount proc busy: PASS",
            "umount invalid target: PASS",
            "mountprobe PASS",
        ],
    ),
]
