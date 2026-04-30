from .common import case


CASES = [
    case(
        name="help",
        must_contain=[
            "shelltest: help begin",
            "Commands:",
            "Programs:",
            "shelltest: help end",
        ],
    )
]
