from .common import case


CASES = [
    case(
        name="memleakprobe",
        command="usr/libexec/tests/memleakprobe",
        must_contain=[
            "memleakprobe workload: PASS",
            "memleakprobe pmm drift: PASS",
            "memleakprobe kalloc drift: PASS",
            "memleakprobe: PASS",
        ],
        timeout=90.0,
    )
]
