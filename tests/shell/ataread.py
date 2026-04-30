from .common import case


CASES = [
    case(
        name="ataread",
        must_contain=[
            "shelltest: ataread begin",
            "sig: 0x55 0xAA",
            "fat16_lba patch: 72",
            "shelltest: ataread end",
        ],
        timeout=60.0,
    )
]
