from .common import case


CASES = [
    case(
        name="inputprobe",
        command="runelf apps/tests/inputprobe",
        must_contain=[
            "inputprobe start",
            "inputprobe waiting for key",
            "inputprobe key ascii=120",
            "inputprobe PASS",
        ],
        interactive=[
            ("inputprobe waiting for key", "x"),
        ],
        timeout=60.0,
    )
]
