from .common import case


CASES = [
    case(
        name="dirprobe",
        command="runelf apps/tests/dirprobe",
        must_contain=[
            "dirprobe start",
            "dir root: PASS",
            "dir nested: PASS",
            "dir eof: PASS",
            "dir invalid file: PASS",
            "dir missing: PASS",
            "dir bad handle: PASS",
            "dirprobe PASS",
        ],
    )
]
