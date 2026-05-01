from .common import case


CASES = [
    case(
        name="fileread",
        command="runelf apps/tests/fileread",
        must_contain=[
            "fileread test",
            "opened fd=3",
            "first 16 bytes: 7F 45 4C 46",
            "close: ok",
            "double-close: ok (got -1)",
            "fread bad fd: ok (got -1)",
        ],
        timeout=60.0,
    )
]
