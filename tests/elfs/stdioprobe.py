from .common import case


CASES = [
    case(
        name="stdioprobe",
        command="runelf usr/libexec/tests/stdioprobe",
        must_contain=[
            "stdioprobe start",
            "stdio eof: PASS",
            "stdio clearerr: PASS",
            "stdio write+fflush: PASS",
            "stdio write failure: PASS",
            "stdio bad read op: PASS",
            "stdio bad fd flush: PASS",
            "stdioprobe PASS",
        ],
    )
]
