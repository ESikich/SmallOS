from .common import case


CASES = [
    case(
        name="bg_ticks",
        must_contain=[
            "shelltest: bg_ticks begin",
            "ticks program",
            "shelltest: bg_ticks end",
        ],
        timeout=60.0,
    )
]
