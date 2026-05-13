from .common import case


CASES = [
    case(
        name="cwdprobe",
        command="runelf usr/libexec/tests/cwdprobe",
        must_contain=[
            "cwdprobe start",
            "cwdprobe cwd=/",
            "cwdprobe cwd=/usr/bin",
            "cwdprobe open relative: PASS",
            "cwdprobe realpath relative: PASS",
            "cwdprobe access relative: PASS",
            "cwdprobe write relative: PASS",
            "cwdprobe PASS",
        ],
    )
]
