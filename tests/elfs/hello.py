from .common import case


CASES = [
    case(
        name="hello",
        command="usr/bin/hello alpha beta",
        must_contain=[
            "hello from user mode via int 0x80",
            "argc = 3",
            'argv[0] = "usr/bin/hello"',
            'argv[1] = "alpha"',
            'argv[2] = "beta"',
        ],
    )
]
