from .common import case


CASES = [
    case(
        name="runelf",
        must_contain=[
            "shelltest: runelf begin",
            "hello from elf via int 0x80",
            'argv[0] = "hello"',
            "shelltest: runelf end",
        ],
        timeout=60.0,
    )
]
