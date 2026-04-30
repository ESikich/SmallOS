from .common import case


CASES = [
    case(
        name="runelf_nowait",
        must_contain=[
            "shelltest: runelf_nowait begin",
            "ticks program",
            "shelltest: runelf_nowait end",
        ],
        timeout=60.0,
    )
]
