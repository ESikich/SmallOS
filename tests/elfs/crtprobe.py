from .common import case


CASES = [
    case(
        name="crtprobe",
        command="runelf apps/tests/crtprobe.elf alpha nested/path longish-argument-0123456789abcdef",
        must_contain=[
            "=== crtprobe PASS ===",
            "crtprobe argv terminator: PASS",
        ],
    )
]
