from .common import case


CASES = [
    case(
        name="echo",
        must_contain=[
            "shelltest: echo begin",
            "alpha beta gamma",
            "shelltest: echo end",
        ],
    )
]
