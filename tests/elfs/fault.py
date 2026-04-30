from .common import case


CASES = [
    case(
        name="fault ud",
        command="runelf fault ud",
        must_contain=[
            "fault: triggering #UD",
            "ud term elf",
        ],
    ),
    case(
        name="fault gp",
        command="runelf fault gp",
        must_contain=[
            "fault: triggering #GP",
            "gp term elf",
        ],
    ),
    case(
        name="fault de",
        command="runelf fault de",
        must_contain=[
            "fault: triggering #DE",
            "de term elf",
        ],
    ),
    case(
        name="fault br",
        command="runelf fault br",
        must_contain=[
            "fault: triggering #BR",
            "br term elf",
        ],
    ),
    case(
        name="fault pf",
        command="runelf fault pf",
        must_contain=[
            "fault: triggering #PF",
            "pf term elf",
        ],
    ),
]
