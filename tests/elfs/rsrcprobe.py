from .common import case


CASES = [
    case(
        "rsrcprobe",
        "usr/libexec/tests/rsrcprobe",
        must_contain=[
            "rsrcprobe start",
            "getrlimit nofile: PASS",
            "setrlimit nofile lower: PASS",
            "nofile enforced: PASS",
            "setrlimit nofile hard fail: PASS",
            "getrlimit as: PASS",
            "getrlimit data: PASS",
            "getrlimit stack: PASS",
            "getrlimit cpu: PASS",
            "getrusage self: PASS",
            "getrusage children: PASS",
            "child accounting wait: PASS",
            "getrusage children after: PASS",
            "rsrcprobe PASS",
        ],
    ),
]
