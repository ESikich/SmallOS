from .common import case


CASES = [
    case(
        name="clear",
        must_contain=[
            "shelltest: clear begin",
            "shelltest: clear end",
        ],
    )
]
