from .common import case


CASES = [
    case(
        name="uptime",
        must_contain=[
            "shelltest: uptime begin",
            "Ticks:",
            "Seconds:",
            "shelltest: uptime end",
        ],
    )
]
