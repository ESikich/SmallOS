from .common import case


CASES = [
    case(
        name="permprobe",
        command="usr/libexec/tests/permprobe",
        must_contain=[
            "permprobe start",
            "root ids: PASS",
            "umask old: PASS",
            "umask create open: PASS",
            "umask create mode: PASS",
            "root private create: PASS",
            "root private write: PASS",
            "mkdir open dir: PASS",
            "mkdir nowrite dir: PASS",
            "wait child: PASS",
            "root cleanup access: PASS",
            "permprobe PASS",
        ],
    )
]
