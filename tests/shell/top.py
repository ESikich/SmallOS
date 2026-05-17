from .common import case


CASES = [
    case(
        name="top",
        must_contain=[
            "shelltest: top begin",
            "top: tasks",
            "PID  PPID ST",
            "RAMK",
            "shelltest: top end",
        ],
        timeout=60.0,
    )
]
