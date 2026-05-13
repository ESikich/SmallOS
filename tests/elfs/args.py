from .common import case


CASES = [
    case(
        name="args",
        command="runelf usr/libexec/tests/args alpha beta",
        must_contain=[
            "argc = 3",
            "argv[0] = usr/libexec/tests/args",
            "argv[1] = alpha",
            "argv[2] = beta",
        ],
    )
]
