from .common import case


CASES = [
    case(
        name="execveprobe",
        command="runelf usr/libexec/tests/execveprobe",
        must_contain=[
            "envprobe start",
            "envprobe argv: PASS",
            "envprobe envp: PASS",
            "envprobe getenv: PASS",
            "envprobe path: PASS",
            "envprobe PASS",
        ],
    )
]
