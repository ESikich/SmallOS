from .common import case


CASES = [
    case(
        name="fileprobe",
        command="runelf apps/tests/fileprobe",
        must_contain=[
            "fileprobe start",
            "elf magic: PASS",
            "write: PASS",
            "stat tmp: PASS",
            "rename: PASS",
            "delete: PASS",
            "fileprobe PASS",
        ],
    )
]
