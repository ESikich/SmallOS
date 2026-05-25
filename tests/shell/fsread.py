from .common import case


CASES = [
    case(
        name="fsread",
        must_contain=[
            "shelltest: fsread begin",
            "fsread: hello",
            "bytes",
            "shelltest: fsread end",
        ],
        timeout=60.0,
    )
]
