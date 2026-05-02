from .common import case


CASES = [
    case(
        name="errnoprobe",
        command="runelf apps/tests/errnoprobe",
        must_contain=[
            "errnoprobe start",
            "raw sys_open missing: PASS",
            "open missing: PASS",
            "close bad fd: PASS",
            "read bad fd: PASS",
            "open directory: PASS",
            "chdir file: PASS",
            "getcwd tiny result: PASS",
            "execvp unsupported: PASS",
            "fd exhaustion count: PASS",
            "fd exhaustion errno: PASS",
            "errnoprobe PASS",
        ],
        timeout=60.0,
    )
]
