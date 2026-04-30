from .common import case


CASES = [
    case(
        name="help",
        must_contain=[
            "shelltest: help begin",
            "Commands:",
            "rm              remove a FAT16 file",
            "cp              copy a FAT16 file",
            "mv              move or rename a FAT16 entry",
            "Programs:",
            "shelltest: help end",
        ],
    )
]
