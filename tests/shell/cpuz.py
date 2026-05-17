from .common import case


CASES = [
    case(
        name="cpuz",
        must_contain=[
            "shelltest: cpuz begin",
            "cpuz: SmallOS hardware summary",
            "CPU",
            "Memory",
            "Display",
            "USB",
            "Network",
            "Storage",
            "shelltest: cpuz end",
        ],
        timeout=60.0,
    )
]
