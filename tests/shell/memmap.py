from .common import case


CASES = [
    case(
        name="memmap",
        must_contain=[
            "shelltest: memmap begin",
            "memmap:",
            "E820 entries",
            "usable",
            "shelltest: memmap end",
        ],
        timeout=60.0,
    )
]
