from .common import case


CASES = [
    case(
        name="hello",
        command="runelf apps/demo/hello alpha beta",
        must_contain=[
            "hello from elf via int 0x80",
            "argc = 3",
            'argv[0] = "apps/demo/hello"',
            'argv[1] = "alpha"',
            'argv[2] = "beta"',
        ],
    )
]
