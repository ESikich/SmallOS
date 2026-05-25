from .common import case


CASES = [
    case(
        name="exec_path",
        must_contain=[
            "shelltest: exec_path begin",
            "hello from user mode via int 0x80",
            "argc = 3",
            'argv[0] = "usr/bin/hello"',
            'argv[1] = "alpha"',
            'argv[2] = "beta"',
            "shelltest: exec_path end",
        ],
        timeout=60.0,
    )
]
