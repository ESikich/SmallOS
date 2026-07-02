from .common import case


CASES = [
    case(
        "sessprobe",
        "usr/libexec/tests/sessprobe",
        must_contain=[
            "sessprobe start",
            "self ids: PASS",
            "getsid by pid: PASS",
            "getpgid by pid: PASS",
            "setpgid self: PASS",
            "setsid leader fail: PASS",
            "wait setsid child: PASS",
            "setpgid child: PASS",
            "tcsetpgrp: PASS",
            "tcgetpgrp: PASS",
            "tcsetpgrp invalid: PASS",
            "sessprobe PASS",
        ],
    ),
]
