from .common import case


CASES = [
    case(
        name="fsls_path",
        must_contain=[
            "shelltest: fsls_path begin",
            "fat16 directory: apps/demo",
            "HELLO.ELF",
            "shelltest: fsls_path end",
        ],
        timeout=60.0,
    )
]
