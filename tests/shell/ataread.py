from .common import case


CASES = [
    case(
        name="ataread",
        must_contain=[
            "shelltest: ataread begin",
            "sig: 0x55 0xAA",
            "fat16 partition lba:",
            "shelltest: ataread end",
        ],
        timeout=60.0,
    )
]
