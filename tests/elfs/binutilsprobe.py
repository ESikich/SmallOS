from .common import case


CASES = [
    case(
        name="binutilsprobe",
        command="usr/libexec/tests/binutilsprobe",
        must_contain=[
            "binutilsprobe start",
            "binutilsprobe write source: PASS",
            "binutilsprobe as: PASS",
            "binutilsprobe ar: PASS",
            "binutilsprobe ranlib: PASS",
            "binutilsprobe ld: PASS",
            "binutilsprobe run linked: PASS",
            "binutilsprobe readelf: PASS",
            "binutilsprobe objdump: PASS",
            "binutilsprobe nm: PASS",
            "binutilsprobe size: PASS",
            "binutilsprobe strings: PASS",
            "binutilsprobe addr2line: PASS",
            "binutilsprobe objcopy: PASS",
            "binutilsprobe run objcopy: PASS",
            "binutilsprobe strip: PASS",
            "binutilsprobe run stripped: PASS",
            "binutilsprobe: PASS",
        ],
        timeout=120.0,
    )
]
