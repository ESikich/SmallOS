from .common import case


CASES = [
    case(
        name="help",
        must_contain=[
            "shelltest: help begin",
            "Commands:",
            "rm              remove an ext2 file",
            "cat             print an ext2 file",
            "touch           create or truncate an ext2 file",
            "cp              copy an ext2 file",
            "mv              move or rename an ext2 entry",
            "cd              change the shell working directory",
            "pwd             print the shell working directory",
            "ls              list an ext2 directory",
            "shelltest: help end",
        ],
    )
]
