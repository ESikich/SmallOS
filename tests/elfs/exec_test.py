from .common import case


CASES = [
    case(
        name="exec_test",
        command="runelf apps/tests/exec_test alpha beta",
        must_contain=[
            "[1] exec_test alive",
            "[2] calling sys_exec apps/demo/hello",
            "[3] sys_exec returned 0",
            'hello from elf via int 0x80',
            'argv[0] = "apps/demo/hello"',
            "[5] bad name returned -2",
            "[7] too many args returned -22",
            "[8] exec_test done",
        ],
        timeout=60.0,
    )
]
