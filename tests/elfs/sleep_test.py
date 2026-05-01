from .common import case


CASES = [
    case(
        name="sleep_test",
        command="runelf apps/tests/sleep_test",
        must_contain=[
            "sleep_test start",
            "sleep_test PASS",
        ],
        timeout=60.0,
    )
]
