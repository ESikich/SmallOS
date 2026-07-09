from .common import case


CASES = [
    case(
        name="execveprobe",
        command="usr/libexec/tests/execveprobe",
        must_contain=[
            "envprobe start",
            "envprobe argv: PASS",
            "envprobe argv budget: PASS",
            "envprobe envp: PASS",
            "envprobe getenv: PASS",
            "envprobe path: PASS",
            "envprobe env budget: PASS",
            "envprobe PASS",
        ],
    )
]
