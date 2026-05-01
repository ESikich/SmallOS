from .common import case


CASES = [
    case(
        name="preempt_test",
        command="runelf apps/tests/preempt_test alpha beta",
        must_contain=[
            "preempt_test start",
            "=== preempt_test PASS ===",
        ],
        timeout=90.0,
    )
]
