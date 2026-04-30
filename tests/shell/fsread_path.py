from .common import case


CASES = [
    case(
        name="fsread_path",
        must_contain=[
            "shelltest: fsread_path begin",
            "fsread: apps/demo/hello.elf",
            "bytes",
            "shelltest: fsread_path end",
        ],
        timeout=60.0,
    )
]
