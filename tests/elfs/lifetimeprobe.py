from .common import case


CASES = [
    case(
        name="lifetimeprobe",
        command="usr/libexec/tests/lifetimeprobe",
        must_contain=[
            "lifetimeprobe warmup: PASS",
            "lifetimeprobe workload: PASS",
            "lifetimeprobe process release: PASS",
            "lifetimeprobe fd/vm release: PASS",
            "lifetimeprobe pmm drift: PASS",
            "lifetimeprobe kalloc drift: PASS",
            "lifetimeprobe: PASS",
        ],
        timeout=120.0,
    )
]
