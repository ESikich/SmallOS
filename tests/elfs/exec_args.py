from .common import case


CASES = [
    case(
        name="exec_args",
        command="usr/libexec/tests/exec_args alpha beta gamma",
        must_contain=[
            "=== exec test PASS ===",
        ],
        timeout=60.0,
    )
]
