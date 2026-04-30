from .common import case


CASES = [
    case(
        name="fsls",
        must_contain=[
            "shelltest: fsls begin",
            "fat16 root directory:",
            "HELLO.ELF",
            "FAULT.ELF",
            "shelltest: fsls end",
        ],
        timeout=60.0,
    )
]
