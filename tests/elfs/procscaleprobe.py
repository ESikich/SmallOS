from .common import case


CASES = [
    case(
        name="procscaleprobe",
        command="usr/libexec/tests/procscaleprobe",
        must_contain=[
            "procscaleprobe process count: PASS",
            "procscaleprobe wait: PASS",
            "procscaleprobe release: PASS",
            "procscaleprobe: PASS",
        ],
        timeout=120.0,
    )
]
