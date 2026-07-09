from .common import case


CASES = [
    case(
        name="stdioprobe",
        command="usr/libexec/tests/stdioprobe",
        must_contain=[
            "stdioprobe start",
            "stdio eof: PASS",
            "stdio clearerr: PASS",
            "stdio write+fflush: PASS",
            "stdio dynamic width: PASS",
            "stdio octal format: PASS",
            "stdio numeric width: PASS",
            "stdio percent m: PASS",
            "stdio unlocked write: PASS",
            "stdio getline unlocked: PASS",
            "stdio write failure: PASS",
            "stdio bad read op: PASS",
            "stdio bad fd flush: PASS",
            "stdioprobe PASS",
        ],
    )
]
