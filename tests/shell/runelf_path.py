from .common import case


CASES = [
    case(
        name="runelf_path",
        must_contain=[
            "shelltest: runelf_path begin",
            "hello from elf via int 0x80",
            "argc = 3",
            'argv[0] = "apps/demo/hello"',
            'argv[1] = "alpha"',
            'argv[2] = "beta"',
            "shelltest: runelf_path end",
        ],
        timeout=60.0,
    )
]
