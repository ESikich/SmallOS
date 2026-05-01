from .common import case


CASES = [
    case(
        name="ticks",
        command="runelf apps/tests/ticks alpha beta",
        must_contain=[
            "ticks program",
            "ticks = ",
        ],
    )
]
