from .common import case


CASES = [
    case(
        name="crtprobe",
        command="runelf usr/libexec/tests/crtprobe.elf alpha nested/path longish-argument-0123456789abcdef",
        must_contain=[
            "=== crtprobe PASS ===",
            "crtprobe argv terminator: PASS",
            "crtprobe envp: PASS",
            "crtprobe PATH: PASS",
        ],
    )
]
