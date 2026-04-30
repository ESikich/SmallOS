from .common import case


CASES = [
    case(
        name="exec_test",
        command="runelf exec_test",
        must_contain=[
            "[1] exec_test alive",
            "[2] calling sys_exec hello",
            "[3] sys_exec returned 0",
            'hello from elf via int 0x80',
            'argv[0] = "hello"',
            "[5] bad name returned -1",
            "[6] exec_test done",
        ],
        timeout=60.0,
    )
]
