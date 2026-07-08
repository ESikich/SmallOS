from .common import case


CASES = [
    case(
        name="fault ud",
        command="usr/libexec/tests/fault ud",
        must_contain=[
            "fault: triggering #UD",
            "ud term fault",
        ],
    ),
    case(
        name="lastfault ud",
        command="cat /proc/lastfault",
        must_contain=[
            "vector: 6",
            "mnemonic: ud",
            "process: fault",
            "mode: user",
        ],
    ),
    case(
        name="fault gp",
        command="usr/libexec/tests/fault gp",
        must_contain=[
            "fault: triggering #GP",
            "gp term fault",
        ],
    ),
    case(
        name="lastfault gp",
        command="cat /proc/lastfault",
        must_contain=[
            "vector: 13",
            "mnemonic: gp",
            "process: fault",
            "mode: user",
        ],
    ),
    case(
        name="fault de",
        command="usr/libexec/tests/fault de",
        must_contain=[
            "fault: triggering #DE",
            "de term fault",
        ],
    ),
    case(
        name="lastfault de",
        command="cat /proc/lastfault",
        must_contain=[
            "vector: 0",
            "mnemonic: de",
            "process: fault",
            "mode: user",
        ],
    ),
    case(
        name="fault br",
        command="usr/libexec/tests/fault br",
        must_contain=[
            "fault: triggering #BR",
            "br term fault",
        ],
    ),
    case(
        name="lastfault br",
        command="cat /proc/lastfault",
        must_contain=[
            "vector: 5",
            "mnemonic: br",
            "process: fault",
            "mode: user",
        ],
    ),
    case(
        name="fault bp",
        command="usr/libexec/tests/fault bp",
        must_contain=[
            "fault: triggering #BP",
            "bp term fault",
        ],
    ),
    case(
        name="lastfault bp",
        command="cat /proc/lastfault",
        must_contain=[
            "vector: 3",
            "mnemonic: bp",
            "process: fault",
            "mode: user",
        ],
    ),
    case(
        name="fault of",
        command="usr/libexec/tests/fault of",
        must_contain=[
            "fault: triggering #OF",
            "of term fault",
        ],
    ),
    case(
        name="lastfault of",
        command="cat /proc/lastfault",
        must_contain=[
            "vector: 4",
            "mnemonic: of",
            "process: fault",
            "mode: user",
        ],
    ),
    case(
        name="fault pf",
        command="usr/libexec/tests/fault pf",
        must_contain=[
            "fault: triggering #PF",
            "pf term fault",
        ],
    ),
    case(
        name="lastfault pf",
        command="cat /proc/lastfault",
        must_contain=[
            "vector: 14",
            "mnemonic: pf",
            "process: fault",
            "mode: user",
            "pf_flags:",
        ],
    ),
]
