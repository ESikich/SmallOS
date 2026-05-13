from .common import case


CASES = [
    case(
        name="cat_touch",
        must_contain=[
            "shelltest: compiler_demo begin",
            "compiler_demo PASS",
            "shelltest: compiler_demo end",
            "shelltest: mkdir_var_tmp begin",
            "mkdir: var/tmp",
            "shelltest: mkdir_var_tmp end",
            "shelltest: mkdir_work begin",
            "shelltest: mkdir_work end",
            "shelltest: cat begin",
            ";; SmallOS compiler demo output",
            "MOV AX, 1",
            "INT 0x80",
            "shelltest: cat end",
            "shelltest: touch begin",
            "touch: var/tmp/EMPTY.TXT",
            "shelltest: touch end",
            "shelltest: fsread_touch begin",
            "fsread: var/tmp/EMPTY.TXT  0 bytes",
            "shelltest: fsread_touch end",
        ],
        timeout=60.0,
    )
]
