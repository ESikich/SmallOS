from .common import case


CASES = [
    case(
        name="fault ud",
        command="runelf usr/libexec/tests/fault ud",
        must_contain=[
            "fault: triggering #UD",
            "ud term elf",
        ],
    ),
    case(
        name="fault gp",
        command="runelf usr/libexec/tests/fault gp",
        must_contain=[
            "fault: triggering #GP",
            "gp term elf",
        ],
    ),
    case(
        name="fault de",
        command="runelf usr/libexec/tests/fault de",
        must_contain=[
            "fault: triggering #DE",
            "de term elf",
        ],
    ),
    case(
        name="fault br",
        command="runelf usr/libexec/tests/fault br",
        must_contain=[
            "fault: triggering #BR",
            "br term elf",
        ],
    ),
    case(
        name="fault pf",
        command="runelf usr/libexec/tests/fault pf",
        must_contain=[
            "fault: triggering #PF",
            "pf term elf",
        ],
    ),
]
