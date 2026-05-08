from .common import case


CASES = [
    case(
        name="meminfo",
        must_contain=[
            "shelltest: meminfo begin",
            "heap:",
            "frames:",
            "used:",
            "e820:",
            "shelltest: meminfo end",
        ],
        timeout=60.0,
    )
]
