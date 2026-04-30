from .common import case


CASES = [
    case(
        name="runelf_test",
        command="runelf runelf_test alpha beta gamma",
        must_contain=[
            "=== runelf test PASS ===",
        ],
        timeout=60.0,
    )
]
