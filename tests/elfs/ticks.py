from .common import case


CASES = [
    case(
        name="ticks",
        command="runelf usr/libexec/tests/ticks alpha beta",
        must_contain=[
            "ticks program",
            "ticks = ",
        ],
    )
]
