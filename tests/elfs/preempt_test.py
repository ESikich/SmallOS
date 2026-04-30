from .common import case


CASES = [
    case(
        name="preempt_test",
        command="runelf preempt_test",
        must_contain=[
            "preempt_test start",
            "=== preempt_test PASS ===",
        ],
        timeout=90.0,
    )
]
