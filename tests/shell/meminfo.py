from .common import case


CASES = [
    case(
        name="meminfo",
        must_contain=[
            "shelltest: meminfo begin",
            "heap:",
            "frames:",
            "used:",
            "shelltest: meminfo end",
        ],
        timeout=60.0,
    )
]
