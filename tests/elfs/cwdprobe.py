from .common import case


CASES = [
    case(
        name="cwdprobe",
        command="runelf apps/tests/cwdprobe",
        must_contain=[
            "cwdprobe start",
            "cwdprobe cwd=/",
            "cwdprobe cwd=/apps/demo",
            "cwdprobe open relative: PASS",
            "cwdprobe realpath relative: PASS",
            "cwdprobe access relative: PASS",
            "cwdprobe write relative: PASS",
            "cwdprobe PASS",
        ],
    )
]
