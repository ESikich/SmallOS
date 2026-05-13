from .common import case


CASES = [
    case(
        name="more",
        must_contain=[
            "shelltest: more_pipe begin",
            "more-pipe-ok",
            "shelltest: more_pipe end",
        ],
    )
]
