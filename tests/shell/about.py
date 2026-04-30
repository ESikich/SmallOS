from .common import case


CASES = [
    case(
        name="about",
        must_contain=[
            "shelltest: about begin",
            "SmallOS v0.1",
            "shelltest: about end",
        ],
    )
]
