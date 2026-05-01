from .common import case


CASES = [
    case(
        name="readline",
        command="runelf apps/tests/readline alpha beta",
        must_contain=[
            "readline test",
            'Hello, erik!',
            'You typed: "smallos"',
            "Length: 7 chars",
        ],
        interactive=[
            ("Enter your name:", "erik"),
            ("Type a line (max 127 chars):", "smallos"),
        ],
        timeout=60.0,
    )
]
