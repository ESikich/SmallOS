from .common import case


CASES = [
    case(
        name="ticks",
        command="runelf ticks",
        must_contain=[
            "ticks program",
            "ticks = ",
        ],
    )
]
